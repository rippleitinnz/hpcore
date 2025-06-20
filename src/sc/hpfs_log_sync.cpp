#include "hpfs_log_sync.hpp"
#include "../util/sequence_hash.hpp"
#include "../conf.hpp"
#include "../crypto.hpp"
#include "../ledger/ledger.hpp"
#include "../msg/fbuf/p2pmsg_conversion.hpp"
#include "../ledger/sqlite.hpp"

namespace p2pmsg = msg::fbuf::p2pmsg;

/**
 * This namespace is responsible for contract state syncing in full history modes. Full history nodes cannot use normal hpfs sync since replay ability should be preserved.
 * Hence log file records are requested from another full history node.
*/
namespace sc::hpfs_log_sync
{
    constexpr int FILE_PERMS = 0644;
    constexpr uint16_t SYNCER_IDLE_WAIT = 20; // log syncer loop sleep time (milliseconds).

    // Max no. of repetitive reqeust resubmissions before abandoning the sync.
    constexpr uint16_t ABANDON_THRESHOLD = 10;

    sync_context sync_ctx;
    bool init_success = false;

    // Represent sequence number and the root hash of the genesis ledger.
    util::sequence_hash genesis_seq_hash;

    /**
     * Initialize log record syncer.
     */
    int init()
    {
        sync_ctx.log_record_sync_thread = std::thread(hpfs_log_syncer_loop);

        genesis_seq_hash = {ledger::genesis.seq_no, hpfs::get_root_hash(ledger::genesis.config_hash, ledger::genesis.state_hash)};

        init_success = true;
        return 0;
    }

    void deinit()
    {
        if (init_success)
        {
            sync_ctx.is_shutting_down = true;
            sync_ctx.log_record_sync_thread.join();
        }
    }

    void set_sync_target(const uint64_t target, const util::h32 &target_root_hash)
    {
        {
            std::scoped_lock lock(sync_ctx.target_log_seq_no_mutex);
            if (sync_ctx.is_shutting_down || (sync_ctx.is_syncing && sync_ctx.target_log_seq_no == target))
                return;

            sync_ctx.target_log_seq_no = target;
            sync_ctx.target_root_hash = target_root_hash;

            const int res = get_verified_min_record();
            // Finding the minimum seq_no to request hpfs logs.
            if (res == -1 || res == 1)
                return;
        }

        LOG_INFO << "Hpfs log sync: Starting sync for target: " << sync_ctx.target_log_seq_no << " min: " << sync_ctx.min_log_record.seq_no;

        sync_ctx.target_requested_on = 0;
        sync_ctx.request_submissions = 0;
        sync_ctx.is_syncing = true;
    }

    /**
     * Runs the log sync worker loop.
     */
    void hpfs_log_syncer_loop()
    {
        util::mask_signal();

        LOG_INFO << "Hpfs log sync: Worker started.";

        while (!sync_ctx.is_shutting_down)
        {
            // Indicates whether any requests/responses were processed in the loop iteration.
            bool processed = false;

            // Perform log sync activities.
            {
                std::scoped_lock<std::mutex> lock(sync_ctx.target_log_seq_no_mutex);
                if (sync_ctx.target_log_seq_no > 0)
                    send_hpfs_log_sync_request(); // Send log record requests if needed (or abandon if sync timeout).

                // Process any hpfs log responses from other nodes.
                if (sync_ctx.target_log_seq_no > 0 && check_hpfs_log_sync_responses() == 1)
                    processed = true;

                // Here we check for the updated log records to check whether target has archived only if any responses have been processed.
                if (sync_ctx.is_syncing && processed && get_verified_min_record() == 1)
                {
                    LOG_INFO << "Hpfs log sync: sync target archived: " << sync_ctx.target_log_seq_no;
                    sync_ctx.clear_target();
                }
            }

            // Serve any hpfs log requests from other nodes.
            if (check_hpfs_log_sync_requests() == 1)
                processed = true;

            // Wait a small delay if there were no requests/responses processed during previous iteration.
            if (!processed)
                util::sleep(SYNCER_IDLE_WAIT);
        }

        LOG_INFO << "Hpfs log sync: Worker stopped.";
    }

    /**
     * Submits/resubmits hpfs log requests as needed. Abandons sync if threshold reached.
     */
    void send_hpfs_log_sync_request()
    {
        // No. of milliseconds to wait before resubmitting a request.
        const uint32_t request_resubmit_timeout = hpfs::get_request_resubmit_timeout();

        // Check whether we need to send any requests or abandon the sync due to timeout.
        const uint64_t time_now = util::get_epoch_milliseconds();
        if ((sync_ctx.target_requested_on == 0) ||                                // Initial request.
            (time_now - sync_ctx.target_requested_on) > request_resubmit_timeout) // Request resubmission.
        {
            if (sync_ctx.request_submissions < ABANDON_THRESHOLD)
            {
                flatbuffers::FlatBufferBuilder fbuf;
                p2pmsg::create_msg_from_hpfs_log_request(fbuf, {sync_ctx.target_log_seq_no, sync_ctx.min_log_record});
                std::string target_pubkey;
                p2p::send_message_to_random_peer(fbuf, target_pubkey, true);
                if (!target_pubkey.empty())
                {
                    LOG_DEBUG << "Hpfs log sync: Requesting from [" << target_pubkey.substr(2, 8) << "]."
                              << " min:" << sync_ctx.min_log_record.seq_no
                              << " target:" << sync_ctx.target_log_seq_no;
                }
                
                sync_ctx.target_requested_on = time_now;
                sync_ctx.request_submissions++;
            }
            else
            {
                LOG_INFO << "Hpfs log sync: Resubmission threshold exceeded. Abandoning sync.";
                sync_ctx.clear_target();
            }
        }
    }

    /**
     * Processes any hpfs log responses we have received from other peers.
     * @return 0 if no respones were processed. 1 if at least one response was processed.
     */
    int check_hpfs_log_sync_responses()
    {
        // Move over the collected responses to the local list.
        std::list<std::pair<std::string, p2p::hpfs_log_response>> hpfs_log_responses;
        {
            std::scoped_lock lock(p2p::ctx.collected_msgs.hpfs_log_response_mutex);

            // Move collected hpfs responses over to local candidate responses list.
            if (!p2p::ctx.collected_msgs.hpfs_log_responses.empty())
                hpfs_log_responses.splice(hpfs_log_responses.end(), p2p::ctx.collected_msgs.hpfs_log_responses);
        }

        for (const auto &[sess_id, log_response] : hpfs_log_responses)
            handle_hpfs_log_sync_response(log_response);

        return hpfs_log_responses.empty() ? 0 : 1;
    }

    /**
     * Serves any hpfs log requests we have received from other peers.
     * @return 0 if no requests were served. 1 if at least one request was served.
     */
    int check_hpfs_log_sync_requests()
    {
        // // Move over the collected requests to the local list.
        std::list<std::pair<std::string, p2p::hpfs_log_request>> log_record_requests;

        {
            std::scoped_lock lock(p2p::ctx.collected_msgs.hpfs_log_request_mutex);

            // Move collected hpfs responses over to local candidate responses list.
            if (!p2p::ctx.collected_msgs.hpfs_log_requests.empty())
                log_record_requests.splice(log_record_requests.end(), p2p::ctx.collected_msgs.hpfs_log_requests);
        }

        for (const auto &[session_id, lr] : log_record_requests)
        {
            // Before serving the request check whether we have the requested min seq_no.
            // And requested min hash matches with our corresponding hash.
            if (!check_required_log_record_availability(lr))
                continue;

            p2p::hpfs_log_response resp;
            if (sc::contract_fs.read_hpfs_logs(lr.min_record_id.seq_no, lr.target_seq_no, resp.log_record_bytes) == -1)
                continue;
            resp.min_record_id = lr.min_record_id;
            flatbuffers::FlatBufferBuilder fbuf(1024);
            p2pmsg::create_msg_from_hpfs_log_response(fbuf, resp);
            std::string_view msg = std::string_view(reinterpret_cast<const char *>(fbuf.GetBufferPointer()), fbuf.GetSize());

            // Find the peer that we should send the history response to.
            std::scoped_lock<std::mutex> lock(p2p::ctx.peer_connections_mutex);
            const auto peer_itr = p2p::ctx.peer_connections.find(util::to_bin(session_id));

            if (peer_itr != p2p::ctx.peer_connections.end())
            {
                comm::comm_session *session = peer_itr->second;
                session->send(msg);
            }
        }

        return log_record_requests.empty() ? 0 : 1;
    }

    /**
     * Check requested sequence number is in node's log file.
     * @param log_request log record request information.
     * @return true if requested sequence number is in node's log file and requested hash mathces with ours.
     */
    bool check_required_log_record_availability(const p2p::hpfs_log_request &log_request)
    {
        // If requested min is the genesis we serve without checking.
        if (log_request.min_record_id == genesis_seq_hash)
            return true;

        util::h32 root_hash;
        if (sc::contract_fs.get_hash_from_index_by_seq_no(root_hash, log_request.min_record_id.seq_no) == -1)
            return false;

        if (root_hash != log_request.min_record_id.hash)
        {
            LOG_DEBUG << "Requested root hash does not match with ours: " << log_request.min_record_id;
            return false;
        }

        return true;
    }

    /**
     * Handle recieved ledger history response.
     * @param log_response log record response information.
     * @return 0 on successful log update. -1 on failure.
     */
    int handle_hpfs_log_sync_response(const p2p::hpfs_log_response &log_response)
    {
        // Append only if the response contains min_seq_no staring from requested min seq_no.
        if (log_response.min_record_id != sync_ctx.min_log_record)
        {
            LOG_DEBUG << "Invalid joining point in the received hpfs log response";
            return -1;
        }

        if (sc::contract_fs.append_hpfs_log_records(log_response.log_record_bytes) == -1)
        {
            LOG_ERROR << "Error persisting hpfs log responses";
            return -1;
        }
        return 0;
    }

    /**
     * Get the verified minimum required ledger.
     * @return -1 on error, 0 on successfully setting minimum target and returns 1 if already in sync.
    */
    int get_verified_min_record()
    {
        util::sequence_hash last_from_index;
        if (sc::contract_fs.get_last_seq_no_from_index(last_from_index.seq_no) == -1 ||
            sc::contract_fs.get_hash_from_index_by_seq_no(last_from_index.hash, last_from_index.seq_no) == -1)
        {
            LOG_ERROR << "Error getting last ledger record data from index file.";
            return -1;
        }

        util::sequence_hash last_from_ledger = ledger::ctx.get_lcl_id();
        if (last_from_index.seq_no == ledger::genesis.seq_no || last_from_ledger.seq_no == ledger::genesis.seq_no)
        {
            // Request full ledger.
            sync_ctx.min_log_record = genesis_seq_hash;
            return 0;
        }

        if (ledger::get_root_hash_from_ledger(last_from_ledger.hash, last_from_ledger.seq_no) == -1)
        {
            LOG_ERROR << "Error getting root hash from ledger for sequence number: " << last_from_index.seq_no;
            return -1;
        }

        if (last_from_index == last_from_ledger)
        {
            // If the last hashes of the ledger and the index are equal, check whether target root hash is equal to the updated root hash in the fs.
            const std::string session_name = "ro_hpfs_log_sync";

            util::h32 state_hash, patch_hash;
            if (sc::contract_fs.start_ro_session(session_name, true) == -1)
            {
                LOG_ERROR << "Hpfs log sync: error starting the hpfs ro session";
                return -1;
            }

            if (sc::contract_fs.get_hash(state_hash, session_name, sc::STATE_DIR_PATH) == -1)
            {
                LOG_ERROR << "Hpfs log sync: error getting the updated state hash";
                sc::contract_fs.stop_ro_session(session_name);
                return -1;
            }

            if (sc::contract_fs.get_hash(patch_hash, session_name, sc::PATCH_FILE_PATH) == -1)
            {
                LOG_ERROR << "Hpfs log sync: error getting the updated patch hash";
                sc::contract_fs.stop_ro_session(session_name);
                return -1;
            }
            sc::contract_fs.stop_ro_session(session_name);

            // If target is equal to the root hash, return 1 so the node in sync, otherwise request hpfs logs from the last ledger seq number.
            if (hpfs::get_root_hash(patch_hash, state_hash) == sync_ctx.target_root_hash)
            {
                // After archiving the target, update the in-memory hash tree.
                sc::contract_fs.set_parent_hash(sc::STATE_DIR_PATH, state_hash);
                sc::contract_fs.set_parent_hash(sc::STATE_DIR_PATH, state_hash);
                return 1;
            }
            else
            {
                // Truncate from the last ledger seq_no. There might be some additional log records after the last index update.
                if (sc::contract_fs.truncate_log_file(last_from_ledger.seq_no) == -1)
                {
                    LOG_ERROR << "Error truncating hpfs log file and index file from : " << last_from_ledger.seq_no;
                    return -1;
                }
                sync_ctx.min_log_record = last_from_ledger;
                return 0;
            }
        }

        if (last_from_index.seq_no == last_from_ledger.seq_no)
        {
            // In a fork because hashes are not equal though the sequence numbers are equal.
            if (set_joining_point_for_fork(last_from_index.seq_no - 1) == -1)
            {
                LOG_ERROR << "Error detecting forked position";
                return -1;
            }
        }
        else if (last_from_ledger.seq_no > last_from_index.seq_no)
        {
            util::h32 root_hash_from_ledger;
            if (ledger::get_root_hash_from_ledger(root_hash_from_ledger, last_from_index.seq_no) == -1)
            {
                LOG_ERROR << "Error getting root hash from ledger for sequence number: " << last_from_index.seq_no;
                return -1;
            }

            if (root_hash_from_ledger == last_from_index.hash)
                sync_ctx.min_log_record = last_from_index;
            else
            {
                // Fork.
                if (set_joining_point_for_fork(last_from_index.seq_no - 1) == -1)
                {
                    LOG_ERROR << "Error detecting forked position";
                    return -1;
                }
            }
        }
        else
        {
            // When index seq is greater than ledger, start from ledger and go back.
            if (set_joining_point_for_fork(last_from_ledger.seq_no - 1) == -1)
            {
                LOG_ERROR << "Error detecting forked position";
                return -1;
            }
        }

        return 0;
    }

    /**
     * Set the joining point as the minimum log record in a case of fork condition by checking index file data
     * against synced ledger data.
     * @param starting_point Starting sequence number to backtrack until a joining state is found. If no joining point is found, min is set to genesis.
     * @return -1 on error and 0 on success.
    */
    int set_joining_point_for_fork(const uint64_t starting_point)
    {
        if (starting_point == 0)
        {
            // Request full ledger.
            sync_ctx.min_log_record = genesis_seq_hash;
            return 0;
        }

        const char *session_name = "ro_get_min_verified_ledger_record";
        if (ledger::ledger_fs.start_ro_session(session_name, false) == -1)
            return -1;

        std::string prev_shard_path;
        sqlite3 *db = NULL;

        util::h32 ledger_root_hash;
        util::h32 index_root_hash;
        uint64_t current_seq_no = starting_point;

        do
        {
            const uint64_t shard_seq_no = SHARD_SEQ(current_seq_no, ledger::PRIMARY_SHARD_SIZE);
            const std::string shard_path = ledger::ledger_fs.physical_path(session_name, ledger::PRIMARY_DIR) + "/" + std::to_string(shard_seq_no);

            // Change db connection if the shard changes.
            if (prev_shard_path != shard_path)
            {
                // Close previous session if any.
                if (db != NULL)
                    ledger::sqlite::close_db(&db);

                if (ledger::sqlite::open_db(shard_path + "/" + ledger::PRIMARY_DB, &db) == -1)
                {
                    LOG_ERROR << errno << ": Error openning the shard database, shard: " << shard_seq_no;
                    ledger::ledger_fs.stop_ro_session(session_name);
                    return -1;
                }
                prev_shard_path = shard_path;
            }

            // Get root hash for the current sequence number from the ledger.
            ledger::ledger_record ledger;
            if (ledger::sqlite::get_ledger_by_seq_no(db, current_seq_no, ledger) == -1)
            {
                LOG_ERROR << "Error getting ledger by sequence number: " << current_seq_no;
                ledger::sqlite::close_db(&db);
                ledger::ledger_fs.stop_ro_session(session_name);
                return -1;
            }
            // Root hash is calculated from its children(patch and state).
            ledger_root_hash = hpfs::get_root_hash(ledger.config_hash, ledger.state_hash);

            // Get root hash for the current seq number from index file.
            if (sc::contract_fs.get_hash_from_index_by_seq_no(index_root_hash, current_seq_no) == -1)
            {
                LOG_ERROR << "Error getting hash from index by sequence number: " << current_seq_no;
                ledger::sqlite::close_db(&db);
                ledger::ledger_fs.stop_ro_session(session_name);
                return -1;
            }

            current_seq_no--;
        } while (current_seq_no > 0 && ledger_root_hash != index_root_hash);

        ledger::sqlite::close_db(&db);
        ledger::ledger_fs.stop_ro_session(session_name);

        // Didn't found a match point until it reaches genesis. Request full ledger.
        if (ledger_root_hash != index_root_hash)
        {
            // Remove the full log and index file data and start from scratch.
            if (sc::contract_fs.truncate_log_file(genesis_seq_hash.seq_no) == -1)
            {
                LOG_ERROR << "Error truncating hpfs log file and index file from : 0";
                return -1;
            }
            // Request full ledger
            sync_ctx.min_log_record = genesis_seq_hash;
        }
        else
        {
            // To account current_seq_no-- at the loop end.
            current_seq_no++;

            if (sc::contract_fs.truncate_log_file(current_seq_no) == -1)
            {
                LOG_ERROR << "Error truncating hpfs log file and index file from : " << current_seq_no;
                return -1;
            }
            // We have found the joining point.
            sync_ctx.min_log_record = {current_seq_no, ledger_root_hash};
        }
        return 0;
    }

} // namespace ledger