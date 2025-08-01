#include "../../util/version.hpp"
#include "../../hpfs/hpfs_mount.hpp"
#include "../../unl.hpp"
#include "../../crypto.hpp"
#include "../../p2p/p2p.hpp"
#include "common_helpers.hpp"
#include "flatbuf_hasher.hpp"
#include "p2pmsg_conversion.hpp"

namespace msg::fbuf::p2pmsg
{
    // Length of a peer connection challange.
    constexpr size_t PEERCHALLENGE_LEN = 16;

    // Max size of messages which are subjected to time (too old) check.
    constexpr size_t MAX_SIZE_FOR_TIME_CHECK = 1 * 1024 * 1024; // 1 MB

    /**
     * This section contains Flatbuffer message reading/writing helpers.
     * A p2p flatbuffer message is a bucket with hp version and the message 'content'.
     */

    //---Flatbuf to std---//

    bool verify_peer_message(std::string_view message)
    {
        // Accessing message buffer
        const uint8_t *buf = reinterpret_cast<const uint8_t *>(message.data());
        const size_t buf_size = message.size();

        // Verify container message using flatbuffer verifier
        flatbuffers::Verifier verifier(buf, buf_size);
        return VerifyP2PMsgBuffer(verifier);
    }

    const p2p::peer_message_info get_peer_message_info(std::string_view message, const p2p::peer_comm_session *session)
    {
        const auto p2p_msg = p2pmsg::GetP2PMsg(message.data());

        // Check message timestamp (ignore this for large messages).
        // Don't perform this check for self messages (session==NULL) or sessions which are still in challenge verification stage.
        if (session && session->challenge_status == comm::CHALLENGE_STATUS::CHALLENGE_VERIFIED && message.size() <= MAX_SIZE_FOR_TIME_CHECK)
        {
            const uint64_t time_now = util::get_epoch_milliseconds();
            if (p2p_msg->created_on() < (time_now - (conf::cfg.contract.consensus.roundtime * 3)))
            {
                LOG_DEBUG << "Peer message is too old. type:" << p2p_msg->content_type() << " from:" << (session ? session->display_name() : "");
                return p2p::peer_message_info{NULL, P2PMsgContent_NONE, 0};
            }
        }

        return p2p::peer_message_info{p2p_msg, p2p_msg->content_type(), p2p_msg->created_on()};
    }

    /**
     * Validate proposal signature against the hash of proposal fields.
     * @return The proposal hash if verification success. Empty hash of verification failed.
     */
    const util::h32 verify_proposal_msg_trust(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_ProposalMsg();

        std::string_view pubkey = flatbuf_bytes_to_sv(msg.pubkey());

        // Before verifying the hash, Validate if the message is from a trusted node.
        if (!unl::exists(std::string(pubkey)))
        {
            LOG_DEBUG << "Peer proposal message pubkey verification failed. Not in UNL.";
            return util::h32_empty;
        }

        // Get hash of proposal data field values and verify the signature against the hash.
        const util::h32 hash = hash_proposal_msg(msg);
        if (crypto::verify(hash.to_string_view(), flatbuf_bytes_to_sv(msg.sig()), pubkey) == 0)
            return hash;
        else
            return util::h32_empty;
    }

    /**
     * Generate a hash using the consensus data fields of the proposal.
     */
    const util::h32 hash_proposal_msg(const msg::fbuf::p2pmsg::ProposalMsg &msg)
    {
        flatbuf_hasher hasher;
        hasher.add(msg.stage());
        hasher.add(msg.time());
        hasher.add(msg.time_config());
        hasher.add(msg.group_nonce());
        hasher.add(msg.users());
        hasher.add(msg.input_hashes());
        hasher.add(msg.output_hash());
        hasher.add(msg.output_sig());
        hasher.add(msg.state_hash());
        hasher.add(msg.patch_hash());
        hasher.add(msg.last_primary_shard_id());
        hasher.add(msg.last_raw_shard_id());
        return hasher.hash();
    }

    bool verify_npl_msg_trust(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_NplMsg();

        std::string_view pubkey = flatbuf_bytes_to_sv(msg.pubkey());

        // Before verifying the hash, Validate if the message is from a trusted node.
        if (!unl::exists(std::string(pubkey)))
        {
            LOG_INFO << "Peer npl message pubkey verification failed. Not in UNL.";
            return false;
        }

        // Get hash of npl message field values and verify the signature against the hash.
        flatbuf_hasher hasher;
        hasher.add(msg.data());
        hasher.add(msg.lcl_id());

        const util::h32 hash = hasher.hash();
        return crypto::verify(hash.to_string_view(), flatbuf_bytes_to_sv(msg.sig()), pubkey) == 0;
    }

    const p2p::peer_challenge create_peer_challenge_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_PeerChallengeMsg();
        return {
            std::string(flatbuf_str_to_sv(msg.contract_id())),
            msg.time_config(),
            msg.is_full_history(),
            std::string(flatbuf_bytes_to_sv(msg.challenge()))};
    }

    const p2p::peer_challenge_response create_peer_challenge_response_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_PeerChallengeResponseMsg();
        return {
            std::string(flatbuf_bytes_to_sv(msg.challenge())),
            std::string(flatbuf_bytes_to_sv(msg.sig())),
            std::string(flatbuf_bytes_to_sv(msg.pubkey()))};
    }

    const p2p::proposal create_proposal_from_msg(const p2p::peer_message_info &mi, const util::h32 &hash)
    {
        const auto &msg = *mi.p2p_msg->content_as_ProposalMsg();

        p2p::proposal p;
        p.pubkey = flatbuf_bytes_to_sv(msg.pubkey());
        p.root_hash = hash;
        p.from_self = p.pubkey == conf::cfg.node.public_key;

        p.sent_timestamp = mi.originated_on;
        p.recv_timestamp = util::get_epoch_milliseconds();
        p.time = msg.time();
        p.time_config = msg.time_config();
        p.node_nonce = flatbuf_bytes_to_sv(msg.node_nonce());
        p.group_nonce = flatbuf_bytes_to_sv(msg.group_nonce());
        p.stage = msg.stage();
        p.state_hash = flatbuf_bytes_to_sv(msg.state_hash());
        p.patch_hash = flatbuf_bytes_to_sv(msg.patch_hash());
        p.last_primary_shard_id = flatbuf_seqhash_to_seqhash(msg.last_primary_shard_id());
        p.last_raw_shard_id = flatbuf_seqhash_to_seqhash(msg.last_raw_shard_id());

        if (msg.users())
            p.users = flatbuf_bytearrayvector_to_stringlist(msg.users());

        if (msg.input_hashes())
            p.input_ordered_hashes = flatbuf_bytearrayvector_to_stringlist(msg.input_hashes());

        if (msg.output_hash())
            p.output_hash = flatbuf_bytes_to_sv(msg.output_hash());

        if (msg.output_sig())
            p.output_sig = flatbuf_bytes_to_sv(msg.output_sig());

        return p;
    }

    const p2p::npl_message create_npl_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_NplMsg();
        return {
            std::string(flatbuf_bytes_to_sv(msg.pubkey())),
            flatbuf_seqhash_to_seqhash(msg.lcl_id()),
            std::string(flatbuf_bytes_to_sv(msg.data()))};
    }

    const p2p::nonunl_proposal create_nonunl_proposal_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_NonUnlProposalMsg();

        p2p::nonunl_proposal nup;
        if (msg.user_inputs())
            nup.user_inputs = flatbuf_user_input_group_to_user_input_map(msg.user_inputs());

        return nup;
    }

    const std::vector<p2p::peer_properties> create_peer_list_response_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_PeerListResponseMsg();
        return flatbuf_peer_propertieslist_to_peer_propertiesvector(msg.peer_list());
    }

    const p2p::peer_capacity_announcement create_peer_capacity_announcement_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_PeerCapacityAnnouncementMsg();
        return {
            msg.available_capacity(),
            msg.timestamp()};
    }

    const p2p::peer_requirement_announcement create_peer_requirement_announcement_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_PeerRequirementAnnouncementMsg();
        return {
            msg.need_consensus_msg_forwarding()};
    }

    const p2p::hpfs_request create_hpfs_request_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_HpfsRequestMsg();
        p2p::hpfs_request hr;
        hr.mount_id = msg.mount_id();
        hr.block_id = msg.block_id();
        hr.is_file = msg.is_file();
        hr.parent_path = flatbuf_str_to_sv(msg.parent_path());
        hr.expected_hash = flatbuf_bytes_to_hash(msg.expected_hash());

        if (msg.hint_type() == HpfsRequestHint_HpfsFsEntryHint)
        {
            flatbuf_hpfsfshashentries_to_hpfsfshashentries(hr.fs_entry_hints, msg.hint_as_HpfsFsEntryHint()->entries());
        }
        else if (msg.hint_type() == HpfsRequestHint_HpfsFileHashMapHint)
        {
            const HpfsFileHashMapHint *hint = msg.hint_as_HpfsFileHashMapHint();
            const size_t block_hash_count = hint->hash_map()->size() / sizeof(util::h32);
            hr.file_hashmap_hints.resize(block_hash_count);
            memcpy(hr.file_hashmap_hints.data(), hint->hash_map()->data(), hint->hash_map()->size());
        }

        return hr;
    }

    const p2p::hpfs_log_request create_hpfs_log_request_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_HpfsLogRequest();
        p2p::hpfs_log_request log_record;
        log_record.target_seq_no = msg.target_seq_no();
        log_record.min_record_id = flatbuf_seqhash_to_seqhash(msg.min_record_id());
        return log_record;
    }

    const p2p::hpfs_log_response create_hpfs_log_response_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_HpfsLogResponse();
        p2p::hpfs_log_response hpfs_log_response;
        hpfs_log_response.min_record_id = flatbuf_seqhash_to_seqhash(msg.min_record_id());
        hpfs_log_response.log_record_bytes.reserve(msg.log_record_bytes()->size());
        for (const auto byte : *msg.log_record_bytes())
            hpfs_log_response.log_record_bytes.push_back(byte);
        return hpfs_log_response;
    }

    util::sequence_hash flatbuf_seqhash_to_seqhash(const SequenceHash *fbseqhash)
    {
        return {
            fbseqhash->seq_no(),
            flatbuf_bytes_to_hash(fbseqhash->hash())};
    }

    const std::set<std::string> flatbuf_bytearrayvector_to_stringlist(const flatbuffers::Vector<flatbuffers::Offset<ByteArray>> *fbvec)
    {
        std::set<std::string> set;
        for (const auto el : *fbvec)
            set.emplace(std::string(flatbuf_bytes_to_sv(el->array())));
        return set;
    }

    const std::unordered_map<std::string, std::list<usr::submitted_user_input>>
    flatbuf_user_input_group_to_user_input_map(const flatbuffers::Vector<flatbuffers::Offset<UserInputGroup>> *fbvec)
    {
        std::unordered_map<std::string, std::list<usr::submitted_user_input>> map;
        map.reserve(fbvec->size());
        for (const UserInputGroup *group : *fbvec)
        {
            std::list<usr::submitted_user_input> user_inputs_list;

            for (const auto msg : *group->messages())
            {
                user_inputs_list.push_back(usr::submitted_user_input{
                    std::string(flatbuf_bytes_to_sv(msg->input_container())),
                    std::string(flatbuf_bytes_to_sv(msg->sig())),
                    static_cast<util::PROTOCOL>(msg->protocol())});
            }

            map.emplace(flatbuf_bytes_to_sv(group->pubkey()), std::move(user_inputs_list));
        }
        return map;
    }

    const p2p::suppress_message create_suppress_from_msg(const p2p::peer_message_info &mi)
    {
        const auto &msg = *mi.p2p_msg->content_as_SuppressMsg();
        return {
            std::string(flatbuf_bytes_to_sv(msg.pubkey())),
            (p2p::SUPPRESS_REASON)msg.reason()};
    }

    void flatbuf_hpfsfshashentries_to_hpfsfshashentries(std::vector<p2p::hpfs_fs_hash_entry> &fs_entries, const flatbuffers::Vector<flatbuffers::Offset<HpfsFSHashEntry>> *fhashes)
    {
        for (const HpfsFSHashEntry *f_hash : *fhashes)
        {
            p2p::hpfs_fs_hash_entry entry;
            entry.name = flatbuf_str_to_sv(f_hash->name());
            entry.is_file = f_hash->is_file();
            entry.hash = flatbuf_bytes_to_hash(f_hash->hash());
            entry.response_type = (p2p::HPFS_FS_ENTRY_RESPONSE_TYPE)f_hash->response_type();

            fs_entries.push_back(std::move(entry));
        }
    }

    const std::vector<p2p::peer_properties>
    flatbuf_peer_propertieslist_to_peer_propertiesvector(const flatbuffers::Vector<flatbuffers::Offset<PeerProperties>> *fbvec)
    {
        std::vector<p2p::peer_properties> peers;

        for (const PeerProperties *peer : *fbvec)
        {
            p2p::peer_properties properties;

            properties.ip_port.host_address = flatbuf_str_to_sv(peer->host_address());
            properties.ip_port.port = peer->port();
            properties.timestamp = peer->timestamp();
            properties.available_capacity = peer->available_capacity();

            peers.push_back(properties);
        }
        return peers;
    }

    //---std to Flatbuf---//

    const std::string generate_proposal_signature(const p2p::proposal &p)
    {
        flatbuf_hasher hasher;
        hasher.add(p.stage);
        hasher.add(p.time);
        hasher.add(p.time_config);
        hasher.add(p.group_nonce);
        hasher.add(p.users);
        hasher.add(p.input_ordered_hashes);
        hasher.add(p.output_hash);
        hasher.add(p.output_sig);
        hasher.add(p.state_hash);
        hasher.add(p.patch_hash);
        hasher.add(p.last_primary_shard_id);
        hasher.add(p.last_raw_shard_id);

        return crypto::sign(hasher.hash().to_string_view(), conf::cfg.node.private_key);
    }

    const std::string generate_npl_signature(std::string_view data, const util::sequence_hash &lcl_id)
    {
        flatbuf_hasher hasher;
        hasher.add(data);
        hasher.add(lcl_id);

        return crypto::sign(hasher.hash().to_string_view(), conf::cfg.node.private_key);
    }

    void create_p2p_msg(flatbuffers::FlatBufferBuilder &builder, const msg::fbuf::p2pmsg::P2PMsgContent content_type, const flatbuffers::Offset<void> content)
    {
        std::string_view version((char *)version::HP_VERSION_BYTES, version::VERSION_BYTES_LEN);
        const auto p2pmsg = CreateP2PMsg(builder,
                                         sv_to_flatbuf_bytes(builder, version),
                                         util::get_epoch_milliseconds(),
                                         content_type,
                                         content);
        builder.Finish(p2pmsg);
    }

    void create_msg_from_peer_challenge(flatbuffers::FlatBufferBuilder &builder, std::string &challenge)
    {
        // We calculate the peer challenge to be a random string.
        crypto::random_bytes(challenge, PEERCHALLENGE_LEN);

        const auto msg = CreatePeerChallengeMsg(
            builder,
            sv_to_flatbuf_str(builder, conf::cfg.contract.id),
            CURRENT_TIME_CONFIG,
            conf::cfg.node.history == conf::HISTORY::FULL,
            sv_to_flatbuf_bytes(builder, challenge));
        create_p2p_msg(builder, P2PMsgContent_PeerChallengeMsg, msg.Union());
    }

    void create_peer_challenge_response_from_challenge(flatbuffers::FlatBufferBuilder &builder, const std::string &challenge)
    {
        const std::string sig = crypto::sign(challenge, conf::cfg.node.private_key);
        const auto msg = CreatePeerChallengeResponseMsg(
            builder,
            sv_to_flatbuf_bytes(builder, challenge),
            sv_to_flatbuf_bytes(builder, conf::cfg.node.public_key),
            sv_to_flatbuf_bytes(builder, sig));

        create_p2p_msg(builder, P2PMsgContent_PeerChallengeResponseMsg, msg.Union());
    }

    void create_msg_from_nonunl_proposal(flatbuffers::FlatBufferBuilder &builder, const p2p::nonunl_proposal &nup)
    {
        const auto msg = CreateNonUnlProposalMsg(
            builder,
            user_input_map_to_flatbuf_user_input_group(builder, nup.user_inputs));

        create_p2p_msg(builder, P2PMsgContent_NonUnlProposalMsg, msg.Union());
    }

    void create_msg_from_proposal(flatbuffers::FlatBufferBuilder &builder, const p2p::proposal &p)
    {
        const auto msg = CreateProposalMsg(
            builder,
            sv_to_flatbuf_bytes(builder, conf::cfg.node.public_key),
            sv_to_flatbuf_bytes(builder, generate_proposal_signature(p)),
            p.stage,
            p.time,
            p.time_config,
            hash_to_flatbuf_bytes(builder, p.node_nonce),
            hash_to_flatbuf_bytes(builder, p.group_nonce),
            stringlist_to_flatbuf_bytearrayvector(builder, p.users),
            stringlist_to_flatbuf_bytearrayvector(builder, p.input_ordered_hashes),
            sv_to_flatbuf_bytes(builder, p.output_hash),
            sv_to_flatbuf_bytes(builder, p.output_sig),
            hash_to_flatbuf_bytes(builder, p.state_hash),
            hash_to_flatbuf_bytes(builder, p.patch_hash),
            seqhash_to_flatbuf_seqhash(builder, p.last_primary_shard_id),
            seqhash_to_flatbuf_seqhash(builder, p.last_raw_shard_id));

        create_p2p_msg(builder, P2PMsgContent_ProposalMsg, msg.Union());
    }

    void create_msg_from_npl_output(flatbuffers::FlatBufferBuilder &builder, std::string_view data, const util::sequence_hash &lcl_id)
    {
        const auto msg = CreateNplMsg(
            builder,
            sv_to_flatbuf_bytes(builder, conf::cfg.node.public_key),
            sv_to_flatbuf_bytes(builder, generate_npl_signature(data, lcl_id)),
            sv_to_flatbuf_bytes(builder, data),
            seqhash_to_flatbuf_seqhash(builder, lcl_id));

        create_p2p_msg(builder, P2PMsgContent_NplMsg, msg.Union());
    }

    void create_msg_from_hpfs_request(flatbuffers::FlatBufferBuilder &builder, const p2p::hpfs_request &hr)
    {
        if (!hr.is_file) // Dir fs entry request.
        {
            const auto hint = CreateHpfsFsEntryHint(
                builder,
                hpfsfshashentry_to_flatbuf_hpfsfshashentry(builder, hr.fs_entry_hints));

            create_hpfs_request_msg(builder, hr, HpfsRequestHint_HpfsFsEntryHint, hint.Union());
        }
        else if (hr.is_file && hr.block_id == -1) // File hash map request.
        {
            std::string_view hashmap_sv(reinterpret_cast<const char *>(hr.file_hashmap_hints.data()), hr.file_hashmap_hints.size() * sizeof(util::h32));

            const auto hint = CreateHpfsFileHashMapHint(
                builder,
                sv_to_flatbuf_bytes(builder, hashmap_sv));

            create_hpfs_request_msg(builder, hr, HpfsRequestHint_HpfsFileHashMapHint, hint.Union());
        }
        else
        {
            create_hpfs_request_msg(builder, hr);
        }
    }

    void create_hpfs_request_msg(flatbuffers::FlatBufferBuilder &builder, const p2p::hpfs_request &hr,
                                 msg::fbuf::p2pmsg::HpfsRequestHint hint_type, flatbuffers::Offset<void> hint)
    {
        const auto msg = CreateHpfsRequestMsg(
            builder,
            hr.mount_id,
            sv_to_flatbuf_str(builder, hr.parent_path),
            hr.is_file,
            hr.block_id,
            hash_to_flatbuf_bytes(builder, hr.expected_hash),
            hint_type,
            hint);

        create_p2p_msg(builder, P2PMsgContent_HpfsRequestMsg, msg.Union());
    }

    void create_msg_from_hpfs_log_request(flatbuffers::FlatBufferBuilder &builder, const p2p::hpfs_log_request &hpfs_log_request)
    {
        const auto msg = CreateHpfsLogRequest(
            builder,
            hpfs_log_request.target_seq_no,
            seqhash_to_flatbuf_seqhash(builder, hpfs_log_request.min_record_id));

        create_p2p_msg(builder, P2PMsgContent_HpfsLogRequest, msg.Union());
    }

    void create_msg_from_hpfs_log_response(flatbuffers::FlatBufferBuilder &builder, const p2p::hpfs_log_response &hpfs_log_response)
    {
        const auto msg = CreateHpfsLogResponse(
            builder,
            seqhash_to_flatbuf_seqhash(builder, hpfs_log_response.min_record_id),
            builder.CreateVector<uint8_t>(hpfs_log_response.log_record_bytes));

        create_p2p_msg(builder, P2PMsgContent_HpfsLogResponse, msg.Union());
    }

    void create_msg_from_fsentry_response(
        flatbuffers::FlatBufferBuilder &builder, const std::string_view path, const uint32_t mount_id, const mode_t dir_mode,
        std::vector<p2p::hpfs_fs_hash_entry> &fs_entries, const util::h32 &expected_hash)
    {
        const auto child_msg = CreateHpfsFsEntryResponse(
            builder,
            dir_mode,
            hpfsfshashentry_to_flatbuf_hpfsfshashentry(builder, fs_entries));

        const auto msg = CreateHpfsResponseMsg(
            builder,
            hash_to_flatbuf_bytes(builder, expected_hash),
            sv_to_flatbuf_str(builder, path),
            mount_id,
            HpfsResponse_HpfsFsEntryResponse,
            child_msg.Union());

        create_p2p_msg(builder, P2PMsgContent_HpfsResponseMsg, msg.Union());
    }

    void create_msg_from_filehashmap_response(
        flatbuffers::FlatBufferBuilder &builder, std::string_view path, const uint32_t mount_id,
        const std::vector<util::h32> &hashmap, const std::vector<uint32_t> &responded_block_ids,
        const std::size_t file_length, const mode_t file_mode, const util::h32 &expected_hash)
    {
        std::string_view hashmap_sv(reinterpret_cast<const char *>(hashmap.data()), hashmap.size() * sizeof(util::h32));

        const auto child_msg = CreateHpfsFileHashMapResponse(
            builder,
            file_length,
            file_mode,
            sv_to_flatbuf_bytes(builder, hashmap_sv),
            builder.CreateVector(responded_block_ids));

        const auto msg = CreateHpfsResponseMsg(
            builder,
            hash_to_flatbuf_bytes(builder, expected_hash),
            sv_to_flatbuf_str(builder, path),
            mount_id,
            HpfsResponse_HpfsFileHashMapResponse,
            child_msg.Union());

        create_p2p_msg(builder, P2PMsgContent_HpfsResponseMsg, msg.Union());
    }

    void create_msg_from_block_response(flatbuffers::FlatBufferBuilder &builder, const uint32_t block_id, const std::vector<uint8_t> &block_data,
                                        const util::h32 &block_hash, std::string_view parent_path, const uint32_t mount_id)
    {
        const auto child_msg = CreateHpfsBlockResponse(
            builder,
            block_id,
            builder.CreateVector(block_data));

        const auto msg = CreateHpfsResponseMsg(
            builder,
            hash_to_flatbuf_bytes(builder, block_hash),
            sv_to_flatbuf_str(builder, parent_path),
            mount_id,
            HpfsResponse_HpfsBlockResponse,
            child_msg.Union());

        create_p2p_msg(builder, P2PMsgContent_HpfsResponseMsg, msg.Union());
    }

    void create_msg_from_peer_requirement_announcement(flatbuffers::FlatBufferBuilder &builder, const bool need_consensus_msg_forwarding)
    {
        const auto msg = CreatePeerRequirementAnnouncementMsg(
            builder,
            need_consensus_msg_forwarding);

        create_p2p_msg(builder, P2PMsgContent_PeerRequirementAnnouncementMsg, msg.Union());
    }

    void create_msg_from_available_capacity_announcement(flatbuffers::FlatBufferBuilder &builder, const int16_t &available_capacity, const uint64_t &timestamp)
    {
        const auto msg = CreatePeerCapacityAnnouncementMsg(
            builder,
            available_capacity,
            timestamp);

        create_p2p_msg(builder, P2PMsgContent_PeerCapacityAnnouncementMsg, msg.Union());
    }

    void create_msg_from_peer_list_request(flatbuffers::FlatBufferBuilder &builder)
    {
        const auto msg = CreatePeerListRequestMsg(builder);
        create_p2p_msg(builder, P2PMsgContent_PeerListRequestMsg, msg.Union());
    }

    void create_msg_from_peer_list_response(flatbuffers::FlatBufferBuilder &builder, const std::vector<p2p::peer_properties> &peers, const std::optional<conf::peer_ip_port> &skipping_ip_port)
    {
        const auto msg = CreatePeerListResponseMsg(
            builder,
            peer_propertiesvector_to_flatbuf_peer_propertieslist(builder, peers, skipping_ip_port));

        create_p2p_msg(builder, P2PMsgContent_PeerListResponseMsg, msg.Union());
    }

    void create_suppress_msg(flatbuffers::FlatBufferBuilder &builder, const uint8_t reason)
    {
        const auto msg = CreateSuppressMsg(
            builder,
            sv_to_flatbuf_bytes(builder, conf::cfg.node.public_key),
            (SuppressReason)reason);

        create_p2p_msg(builder, P2PMsgContent_SuppressMsg, msg.Union());
    }

    const flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<UserInputGroup>>>
    user_input_map_to_flatbuf_user_input_group(flatbuffers::FlatBufferBuilder &builder, const std::unordered_map<std::string, std::list<usr::submitted_user_input>> &map)
    {
        std::vector<flatbuffers::Offset<UserInputGroup>> fbvec;
        fbvec.reserve(map.size());
        for (const auto &[pubkey, msglist] : map)
        {
            std::vector<flatbuffers::Offset<UserInput>> fbmsgsvec;
            for (const usr::submitted_user_input &msg : msglist)
            {
                fbmsgsvec.push_back(CreateUserInput(
                    builder,
                    sv_to_flatbuf_bytes(builder, msg.input_container),
                    sv_to_flatbuf_bytes(builder, msg.sig),
                    static_cast<uint8_t>(msg.protocol)));
            }

            fbvec.push_back(CreateUserInputGroup(
                builder,
                sv_to_flatbuf_bytes(builder, pubkey),
                builder.CreateVector(fbmsgsvec)));
        }
        return builder.CreateVector(fbvec);
    }

    const flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<HpfsFSHashEntry>>>
    hpfsfshashentry_to_flatbuf_hpfsfshashentry(flatbuffers::FlatBufferBuilder &builder, const std::vector<p2p::hpfs_fs_hash_entry> &fs_entries)
    {
        std::vector<flatbuffers::Offset<HpfsFSHashEntry>> fbvec;
        fbvec.reserve(fs_entries.size());
        for (auto const &fs_entry : fs_entries)
        {
            flatbuffers::Offset<HpfsFSHashEntry> hpfs_fs_entry = CreateHpfsFSHashEntry(
                builder,
                sv_to_flatbuf_str(builder, fs_entry.name),
                fs_entry.is_file,
                hash_to_flatbuf_bytes(builder, fs_entry.hash),
                (HpfsFsEntryResponseType)fs_entry.response_type);

            fbvec.push_back(hpfs_fs_entry);
        }
        return builder.CreateVector(fbvec);
    }

    const flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<PeerProperties>>>
    peer_propertiesvector_to_flatbuf_peer_propertieslist(flatbuffers::FlatBufferBuilder &builder, const std::vector<p2p::peer_properties> &peers, const std::optional<conf::peer_ip_port> &skipping_ip_port)
    {
        std::vector<flatbuffers::Offset<PeerProperties>> fbvec;
        fbvec.reserve(peers.size());
        for (const auto &peer : peers)
        {
            if (peer.ip_port.host_address.empty())
            {
                LOG_DEBUG << "Skip sending peer with blank host address : " << peer.ip_port.to_string();
                continue;
            }

            // Skipping the requestedc peer from the peer list response.
            if (!skipping_ip_port.has_value() || peer.ip_port != skipping_ip_port.value())
                fbvec.push_back(CreatePeerProperties(
                    builder,
                    sv_to_flatbuf_str(builder, peer.ip_port.host_address),
                    peer.ip_port.port,
                    peer.available_capacity,
                    peer.timestamp));
        }
        return builder.CreateVector(fbvec);
    }

    const flatbuffers::Offset<msg::fbuf::p2pmsg::SequenceHash>
    seqhash_to_flatbuf_seqhash(flatbuffers::FlatBufferBuilder &builder, const util::sequence_hash &seqhash)
    {
        return CreateSequenceHash(builder, seqhash.seq_no, hash_to_flatbuf_bytes(builder, seqhash.hash));
    }

    const flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ByteArray>>>
    stringlist_to_flatbuf_bytearrayvector(flatbuffers::FlatBufferBuilder &builder, const std::set<std::string> &set)
    {
        std::vector<flatbuffers::Offset<ByteArray>> fbvec;
        fbvec.reserve(set.size());
        for (std::string_view str : set)
            fbvec.push_back(CreateByteArray(builder, sv_to_flatbuf_bytes(builder, str)));
        return builder.CreateVector(fbvec);
    }
}