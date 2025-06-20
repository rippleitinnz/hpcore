
#include "./contract_sync.hpp"
#include "../unl.hpp"
#include "../hpfs/hpfs_mount.hpp"
#include "contract_mount.hpp"
#include "../consensus.hpp"

namespace sc
{

    void contract_sync::on_sync_target_acheived(const std::string &vpath, const util::h32 &hash)
    {
        if (vpath == PATCH_FILE_PATH)
        {
            // Appling new patch file changes to hpcore runtime.
            if (conf::apply_patch_config(hpfs::RW_SESSION_NAME) == -1)
            {
                LOG_ERROR << "Appling patch file changes after sync failed";
            }
            else
            {
                unl::update_unl_changes_from_patch();
                consensus::refresh_time_config(false);

                // Update global hash tracker with the new patch file hash.
                fs_mount->set_parent_hash(vpath, hash);
            }
        }
    }

    void contract_sync::swap_collected_responses()
    {
        std::scoped_lock lock(p2p::ctx.collected_msgs.contract_hpfs_responses_mutex);

        // Move collected hpfs responses over to local candidate responses list.
        if (!p2p::ctx.collected_msgs.contract_hpfs_responses.empty())
            candidate_hpfs_responses.splice(candidate_hpfs_responses.end(), p2p::ctx.collected_msgs.contract_hpfs_responses);
    }
} // namespace sc