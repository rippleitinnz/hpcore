#ifndef _HP_LEDGER_LEDGER_QUERY_
#define _HP_LEDGER_LEDGER_QUERY_

#include "../pchheader.hpp"
#include "ledger_common.hpp"

namespace ledger::query
{
    /**
     * Represents a ledger query request to filter by seq no.
     */
    struct seq_no_query
    {
        uint64_t seq_no = 0;
        bool inputs = false;
        bool outputs = false;
    };

    struct user_buffer_collection
    {
        std::string pubkey;               // Binary user pubkey.
        std::vector<std::string> buffers; // List of binary data buffers.
    };

    typedef std::variant<seq_no_query> query_request;
    typedef std::variant<const char *, std::vector<ledger::ledger_record>> query_result;

    const query_result execute(std::string_view user_pubkey, const query_request &q);
    int get_ledger_by_seq_no(ledger_record &ledger, const seq_no_query &q, const std::string &fs_sess_name);
    int get_ledger_raw_data(ledger_record &ledger, std::string_view user_pubkey, const std::string &fs_sess_name);
    int get_ledger_inputs(sqlite3 *db, std::vector<ledger_user_input> &inputs, const uint64_t seq_no, const std::string &shard_path, std::string_view user_pubkey, const std::string &fs_sess_name);
    int get_ledger_outputs(sqlite3 *db, std::vector<ledger_user_output> &outputs, const uint64_t seq_no, const std::string &shard_path, std::string_view user_pubkey, const std::string &fs_sess_name);
    int get_input_users_from_ledger(const uint64_t seq_no, std::vector<std::string> &users, std::vector<ledger_user_input> &inputs);
    int get_input_by_hash(const uint64_t lcl_seq_no, std::string_view hash, std::optional<ledger::ledger_user_input> &input, std::optional<ledger::ledger_record> &ledger);
}

#endif