#ifndef _HP_UNL_
#define _HP_UNL_

#include "pchheader.hpp"
#include "p2p/p2p.hpp"
#include "usr/usr.hpp"

/**
 * Manages the UNL public keys of this node.
 */
namespace unl
{

    size_t count();
    const std::set<std::string> get();
    const std::string get_json();
    bool exists(const std::string &bin_pubkey);
    int init();
    void update_unl_changes_from_patch();
    void update_unl_stats(const std::list<p2p::proposal> &proposals);
    uint32_t get_majority_time_config();
    bool merge_latest_unl_config();
    const std::string prepare_json_list();

} // namespace unl

#endif
