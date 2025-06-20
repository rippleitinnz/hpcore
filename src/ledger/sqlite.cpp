#include "sqlite.hpp"
#include "../util/h32.hpp"
#include "ledger_common.hpp"

namespace ledger::sqlite
{
    constexpr const char *LEDGER_TABLE = "ledger";
    constexpr const char *USERS_TABLE = "users";
    constexpr const char *INPUTS_TABLE = "inputs";
    constexpr const char *OUTPUTS_TABLE = "outputs";
    constexpr const char *HP_TABLE = "hp";
    constexpr const char *LEDGER_VERSION_COLUMN = "ledger_version";
    constexpr const char *COLUMN_DATA_TYPES[]{"INT", "TEXT", "BLOB"};
    constexpr const char *CREATE_TABLE = "CREATE TABLE IF NOT EXISTS ";
    constexpr const char *CREATE_INDEX = "CREATE INDEX ";
    constexpr const char *CREATE_UNIQUE_INDEX = "CREATE UNIQUE INDEX ";
    constexpr const char *JOURNAL_MODE_OFF = "PRAGMA journal_mode=OFF";
    constexpr const char *BEGIN_TRANSACTION = "BEGIN TRANSACTION;";
    constexpr const char *COMMIT_TRANSACTION = "COMMIT;";
    constexpr const char *ROLLBACK_TRANSACTION = "ROLLBACK;";
    constexpr const char *INSERT_INTO = "INSERT INTO ";
    constexpr const char *PRIMARY_KEY = "PRIMARY KEY";
    constexpr const char *NOT_NULL = "NOT NULL";
    constexpr const char *VALUES = "VALUES";
    constexpr const char *SELECT_ALL = "SELECT * FROM ";
    constexpr const char *SQLITE_MASTER = "sqlite_master";
    constexpr const char *WHERE = " WHERE ";
    constexpr const char *AND = " AND ";
    constexpr const char *SELECT_LAST_LEDGER = "SELECT * FROM ledger ORDER BY seq_no DESC LIMIT 1";
    constexpr const char *SELECT_LEDGER_BY_SEQ_NO = "SELECT * FROM ledger WHERE seq_no=? LIMIT 1";
    constexpr const char *SELECT_USERS_BY_SEQ_NO = "SELECT * FROM users WHERE ledger_seq_no=?";
    constexpr const char *SELECT_INPUTS_BY_SEQ_NO = "SELECT * FROM inputs WHERE ledger_seq_no=?";
    constexpr const char *SELECT_OUTPUTS_BY_SEQ_NO = "SELECT * FROM outputs WHERE ledger_seq_no=?";
    constexpr const char *SELECT_INPUT_BY_HASH = "SELECT * FROM inputs WHERE hash=?";

    constexpr const char *INSERT_INTO_LEDGER = "INSERT INTO ledger("
                                               "seq_no, time, ledger_hash, prev_ledger_hash, data_hash,"
                                               "state_hash, config_hash, nonce, user_hash, input_hash, output_hash"
                                               ") VALUES(?,?,?,?,?,?,?,?,?,?,?)";
    constexpr const char *INSERT_INTO_USERS = "INSERT INTO users(ledger_seq_no, pubkey) VALUES(?,?)";
    constexpr const char *INSERT_INTO_USER_INPUTS = "INSERT INTO inputs(ledger_seq_no, pubkey, hash, nonce,"
                                                    " blob_offset, blob_size) VALUES(?,?,?,?,?,?)";
    constexpr const char *INSERT_INTO_USER_OUTPUTS = "INSERT INTO outputs(ledger_seq_no, pubkey, hash,"
                                                     " blob_offset, blob_count) VALUES(?,?,?,?,?)";

#define PUBKEY_SIZE 33
#define BIND_H32_BLOB(idx, field) (field.size() == sizeof(util::h32) && sqlite3_bind_blob(stmt, idx, field.data(), sizeof(util::h32), SQLITE_STATIC) == SQLITE_OK)
#define BIND_PUBKEY_BLOB(idx, field) (field.size() == PUBKEY_SIZE && sqlite3_bind_blob(stmt, idx, field.data(), PUBKEY_SIZE, SQLITE_STATIC) == SQLITE_OK)
#define GET_H32_BLOB(idx) std::string((char *)sqlite3_column_blob(stmt, idx), sizeof(util::h32))
#define GET_PUBKEY_BLOB(idx) std::string((char *)sqlite3_column_blob(stmt, idx), PUBKEY_SIZE)

    /**
     * Opens a connection to a given databse and give the db pointer.
     * @param db_name Database name to be connected.
     * @param db Pointer to the db pointer which is to be connected and pointed.
     * @param writable Whether the database must be opened in a writable mode or not.
     * @param journal Whether to enable db journaling or not.
     * @returns returns 0 on success, or -1 on error.
     */
    int open_db(std::string_view db_name, sqlite3 **db, const bool writable, const bool journal)
    {
        int ret;
        const int flags = writable ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
        if ((ret = sqlite3_open_v2(db_name.data(), db, flags, 0)) != SQLITE_OK)
        {
            LOG_ERROR << ret << ": Sqlite error when opening database " << db_name;
            *db = NULL;
            return -1;
        }

        // We can turn off journaling for the db if we don't need transacion support.
        // Journaling mode can introduce lot of extra underyling file system operations which may cause
        // lot of overhead if used on a low-performance filesystem like hpfs.
        if (writable && !journal && exec_sql(*db, JOURNAL_MODE_OFF) == -1)
            return -1;

        return 0;
    }

    /**
     * Executes given sql query.
     * @param db Pointer to the db.
     * @param sql Sql query to be executed.
     * @param callback Callback funcion which is called for each result row.
     * @param callback_first_arg First data argumat to be parced to the callback (void pointer).
     * @returns returns 0 on success, or -1 on error.
     */
    int exec_sql(sqlite3 *db, std::string_view sql, int (*callback)(void *, int, char **, char **), void *callback_first_arg)
    {
        char *err_msg;
        if (sqlite3_exec(db, sql.data(), callback, (callback != NULL ? (void *)callback_first_arg : NULL), &err_msg) != SQLITE_OK)
        {
            LOG_ERROR << "SQL error occured: " << err_msg;
            sqlite3_free(err_msg);
            return -1;
        }
        return 0;
    }

    int begin_transaction(sqlite3 *db)
    {
        return sqlite::exec_sql(db, BEGIN_TRANSACTION);
    }

    int commit_transaction(sqlite3 *db)
    {
        return sqlite::exec_sql(db, COMMIT_TRANSACTION);
    }

    int rollback_transaction(sqlite3 *db)
    {
        return sqlite::exec_sql(db, ROLLBACK_TRANSACTION);
    }

    /**
     * Create a table with given table info.
     * @param db Pointer to the db.
     * @param table_name Table name to be created.
     * @param column_info Column info of the table.
     * @returns returns 0 on success, or -1 on error.
     */
    int create_table(sqlite3 *db, std::string_view table_name, const std::vector<table_column_info> &column_info)
    {
        std::string sql;
        sql.append(CREATE_TABLE).append(table_name).append(" (");

        for (auto itr = column_info.begin(); itr != column_info.end(); ++itr)
        {
            sql.append(itr->name);
            sql.append(" ");
            sql.append(COLUMN_DATA_TYPES[itr->column_type]);

            if (itr->is_key)
            {
                sql.append(" ");
                sql.append(PRIMARY_KEY);
            }

            if (!itr->is_null)
            {
                sql.append(" ");
                sql.append(NOT_NULL);
            }

            if (itr != column_info.end() - 1)
                sql.append(",");
        }
        sql.append(")");

        const int ret = exec_sql(db, sql);
        if (ret == -1)
        {
            LOG_ERROR << "Error when creating sqlite table " << table_name;
        }

        return ret;
    }

    int create_index(sqlite3 *db, std::string_view table_name, std::string_view column_names, const bool is_unique)
    {
        std::string index_name = std::string("idx_").append(table_name).append("_").append(column_names);
        std::replace(index_name.begin(), index_name.end(), ',', '_');

        std::string sql;
        sql.append(is_unique ? CREATE_UNIQUE_INDEX : CREATE_INDEX)
            .append(index_name)
            .append(" ON ")
            .append(table_name)
            .append("(")
            .append(column_names)
            .append(")");

        const int ret = exec_sql(db, sql);
        if (ret == -1)
        {
            LOG_ERROR << "Error when creating sqlite index '" << index_name << "' in table " << table_name;
        }

        return ret;
    }

    /**
     * Inserts mulitple rows to a table.
     * @param db Pointer to the db.
     * @param table_name Table name to be populated.
     * @param column_names_string Comma seperated string of colums (eg: "col_1,col_2,...").
     * @param value_strings Vector of comma seperated values (wrap in single quotes for TEXT type) (eg: ["r1val1,'r1val2',...", "r2val1,'r2val2',..."]).
     * @returns returns 0 on success, or -1 on error.
     */
    int insert_rows(sqlite3 *db, std::string_view table_name, std::string_view column_names_string, const std::vector<std::string> &value_strings)
    {
        std::string sql;

        sql.append(INSERT_INTO);
        sql.append(table_name);
        sql.append("(");
        sql.append(column_names_string);
        sql.append(") ");
        sql.append(VALUES);

        for (auto itr = value_strings.begin(); itr != value_strings.end(); ++itr)
        {
            sql.append("(");
            sql.append(*itr);
            sql.append(")");

            if (itr != value_strings.end() - 1)
                sql.append(",");
        }

        /* Execute SQL statement */
        return exec_sql(db, sql);
    }

    /**
     * Inserts a row to a table.
     * @param db Pointer to the db.
     * @param table_name Table name to be populated.
     * @param column_names_string Comma seperated string of colums (eg: "col_1,col_2,...").
     * @param value_string comma seperated values as per column order (wrap in single quotes for TEXT type) (eg: "r1val1,'r1val2',...").
     * @returns returns 0 on success, or -1 on error.
     */
    int insert_row(sqlite3 *db, std::string_view table_name, std::string_view column_names_string, std::string_view value_string)
    {
        std::string sql;
        // Reserving the space for the query before construction.
        sql.reserve(sizeof(INSERT_INTO) + table_name.size() + column_names_string.size() + sizeof(VALUES) + value_string.size() + 5);

        sql.append(INSERT_INTO);
        sql.append(table_name);
        sql.append("(");
        sql.append(column_names_string);
        sql.append(") ");
        sql.append(VALUES);
        sql.append("(");
        sql.append(value_string);
        sql.append(")");

        /* Execute SQL statement */
        return exec_sql(db, sql);
    }

    /**
     * Checks whether table exist in the database.
     * @param db Pointer to the db.
     * @param table_name Table name to be checked.
     * @returns returns true is exist, otherwise false.
     */
    bool is_table_exists(sqlite3 *db, std::string_view table_name)
    {
        std::string sql;
        // Reserving the space for the query before construction.
        sql.reserve(sizeof(SELECT_ALL) + sizeof(SQLITE_MASTER) + sizeof(WHERE) + sizeof(AND) + table_name.size() + 19);

        sql.append(SELECT_ALL);
        sql.append(SQLITE_MASTER);
        sql.append(WHERE);
        sql.append("type='table'");
        sql.append(AND);
        sql.append("name='");
        sql.append(table_name);
        sql.append("'");

        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, sql.data(), -1, &stmt, 0) == SQLITE_OK &&
            stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
        {
            // Finalize and distroys the statement.
            sqlite3_finalize(stmt);
            return true;
        }

        // Finalize and distroys the statement.
        sqlite3_finalize(stmt);
        return false;
    }

    /**
     * Closes a connection to a given databse.
     * @param db Pointer to the db.
     * @returns returns 0 on success, or -1 on error.
     */
    int close_db(sqlite3 **db)
    {
        if (*db == NULL)
            return 0;

        if (sqlite3_close(*db) != SQLITE_OK)
        {
            LOG_ERROR << "Can't close database: " << sqlite3_errmsg(*db);
            return -1;
        }

        *db = NULL;
        return 0;
    }

    /**
     * Sets up a blank ledger database.
     * @param db Pointer to the db.
     * @returns returns 0 on success, or -1 on error.
     */
    int initialize_ledger_db(sqlite3 *db)
    {
        const std::vector<table_column_info> columns{
            table_column_info("seq_no", COLUMN_DATA_TYPE::INT, true),
            table_column_info("time", COLUMN_DATA_TYPE::INT),
            table_column_info("ledger_hash", COLUMN_DATA_TYPE::BLOB),
            table_column_info("prev_ledger_hash", COLUMN_DATA_TYPE::BLOB),
            table_column_info("data_hash", COLUMN_DATA_TYPE::BLOB),
            table_column_info("state_hash", COLUMN_DATA_TYPE::BLOB),
            table_column_info("config_hash", COLUMN_DATA_TYPE::BLOB),
            table_column_info("nonce", COLUMN_DATA_TYPE::BLOB),
            table_column_info("user_hash", COLUMN_DATA_TYPE::BLOB),
            table_column_info("input_hash", COLUMN_DATA_TYPE::BLOB),
            table_column_info("output_hash", COLUMN_DATA_TYPE::BLOB)};

        if (create_table(db, LEDGER_TABLE, columns) == -1 ||
            create_index(db, LEDGER_TABLE, "time", true) == -1 ||
            create_index(db, LEDGER_TABLE, "ledger_hash", true) == -1)
            return -1;

        return 0;
    }

    /**
     * Sets up a blank ledger raw data database.
     * @param db Pointer to the db.
     * @returns returns 0 on success, or -1 on error.
     */
    int initialize_ledger_raw_db(sqlite3 *db)
    {
        // Users table.
        {
            const std::vector<table_column_info> user_columns{
                table_column_info("ledger_seq_no", COLUMN_DATA_TYPE::INT),
                table_column_info("pubkey", COLUMN_DATA_TYPE::BLOB)};

            if (create_table(db, USERS_TABLE, user_columns) == -1 ||
                create_index(db, USERS_TABLE, "ledger_seq_no", false) == -1 ||
                create_index(db, USERS_TABLE, "pubkey", false) == -1)
                return -1;
        }

        // Inputs table.
        {
            const std::vector<table_column_info> input_columns{
                table_column_info("ledger_seq_no", COLUMN_DATA_TYPE::INT),
                table_column_info("pubkey", COLUMN_DATA_TYPE::BLOB),
                table_column_info("hash", COLUMN_DATA_TYPE::BLOB),
                table_column_info("nonce", COLUMN_DATA_TYPE::INT),
                table_column_info("blob_offset", COLUMN_DATA_TYPE::INT),
                table_column_info("blob_size", COLUMN_DATA_TYPE::INT)};

            if (create_table(db, INPUTS_TABLE, input_columns) == -1 ||
                create_index(db, INPUTS_TABLE, "ledger_seq_no", false) == -1 ||
                create_index(db, INPUTS_TABLE, "hash", false) == -1 ||
                create_index(db, INPUTS_TABLE, "ledger_seq_no,pubkey", false) == -1)
                return -1;
        }

        // Outputs table.
        {
            const std::vector<table_column_info> input_columns{
                table_column_info("ledger_seq_no", COLUMN_DATA_TYPE::INT),
                table_column_info("pubkey", COLUMN_DATA_TYPE::BLOB),
                table_column_info("hash", COLUMN_DATA_TYPE::BLOB),
                table_column_info("blob_offset", COLUMN_DATA_TYPE::INT),
                table_column_info("blob_count", COLUMN_DATA_TYPE::INT)};

            if (create_table(db, OUTPUTS_TABLE, input_columns) == -1 ||
                create_index(db, OUTPUTS_TABLE, "ledger_seq_no", false) == -1 ||
                create_index(db, OUTPUTS_TABLE, "hash", false) == -1 ||
                create_index(db, OUTPUTS_TABLE, "ledger_seq_no,pubkey", false) == -1)
                return -1;
        }

        return 0;
    }

    /**
     * Create and update the hp system table when creating a new shard.
     * @param db Pointer to the db.
     * @param version Version string to be placed in the table.
     * @returns returns 0 on success, or -1 on error.
     *
     */
    int create_hp_table(sqlite3 *db, std::string_view version)
    {
        const std::vector<table_column_info> column_info{
            table_column_info(LEDGER_VERSION_COLUMN, COLUMN_DATA_TYPE::TEXT)};

        if (create_table(db, HP_TABLE, column_info) == -1)
            return -1;

        const std::string value_string = "\"" + std::string(version) + "\"";
        if (insert_row(db, HP_TABLE, LEDGER_VERSION_COLUMN, value_string) == -1)
            return -1;

        return 0;
    }

    /**
     * Inserts a ledger record.
     * @param db Pointer to the db.
     * @param ledger Ledger struct to be inserted.
     * @returns returns 0 on success, or -1 on error.
     */
    int insert_ledger_row(sqlite3 *db, const ledger::ledger_record &ledger)
    {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, INSERT_INTO_LEDGER, -1, &stmt, 0) == SQLITE_OK && stmt != NULL &&
            sqlite3_bind_int64(stmt, 1, ledger.seq_no) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 2, ledger.timestamp) == SQLITE_OK &&
            BIND_H32_BLOB(3, ledger.ledger_hash) &&
            BIND_H32_BLOB(4, ledger.prev_ledger_hash) &&
            BIND_H32_BLOB(5, ledger.data_hash) &&
            BIND_H32_BLOB(6, ledger.state_hash) &&
            BIND_H32_BLOB(7, ledger.config_hash) &&
            BIND_H32_BLOB(8, ledger.nonce) &&
            BIND_H32_BLOB(9, ledger.user_hash) &&
            BIND_H32_BLOB(10, ledger.input_hash) &&
            BIND_H32_BLOB(11, ledger.output_hash) &&
            sqlite3_step(stmt) == SQLITE_DONE)
        {
            sqlite3_finalize(stmt);
            return 0;
        }

        LOG_ERROR << "Error inserting ledger record. " << sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_stmt *prepare_user_insert(sqlite3 *db)
    {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, INSERT_INTO_USERS, -1, &stmt, 0) == SQLITE_OK && stmt != NULL)
            return stmt;

        return NULL;
    }

    sqlite3_stmt *prepare_user_input_insert(sqlite3 *db)
    {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, INSERT_INTO_USER_INPUTS, -1, &stmt, 0) == SQLITE_OK && stmt != NULL)
            return stmt;

        return NULL;
    }

    sqlite3_stmt *prepare_user_output_insert(sqlite3 *db)
    {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, INSERT_INTO_USER_OUTPUTS, -1, &stmt, 0) == SQLITE_OK && stmt != NULL)
            return stmt;

        LOG_ERROR << "Prepare sqlite statement failed.";
        return NULL;
    }

    int insert_user_record(sqlite3_stmt *stmt, const uint64_t ledger_seq_no, std::string_view pubkey)
    {
        if (stmt == NULL)
        {
            LOG_ERROR << "Sqlite statement null.";
            return -1;
        }

        if (sqlite3_reset(stmt) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 1, ledger_seq_no) == SQLITE_OK &&
            BIND_PUBKEY_BLOB(2, pubkey) &&
            sqlite3_step(stmt) == SQLITE_DONE)
        {
            return 0;
        }

        LOG_ERROR << "Error inserting user record.";
        return -1;
    }

    int insert_user_input_record(sqlite3_stmt *stmt, const uint64_t ledger_seq_no, std::string_view pubkey,
                                 std::string_view hash, const uint64_t nonce, const uint64_t blob_offset, const uint64_t blob_size)
    {
        if (stmt == NULL)
        {
            LOG_ERROR << "Sqlite statement null.";
            return -1;
        }

        if (sqlite3_reset(stmt) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 1, ledger_seq_no) == SQLITE_OK &&
            BIND_PUBKEY_BLOB(2, pubkey) &&
            BIND_H32_BLOB(3, hash) &&
            sqlite3_bind_int64(stmt, 4, nonce) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 5, blob_offset) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 6, blob_size) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_DONE)
        {
            return 0;
        }

        LOG_ERROR << "Error inserting user input record.";
        return -1;
    }

    int insert_user_output_record(sqlite3_stmt *stmt, const uint64_t ledger_seq_no, std::string_view pubkey,
                                  std::string_view hash, const uint64_t blob_offset, const uint64_t output_count)
    {
        if (stmt == NULL)
        {
            LOG_ERROR << "Sqlite statement null.";
            return -1;
        }

        if (sqlite3_reset(stmt) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 1, ledger_seq_no) == SQLITE_OK &&
            BIND_PUBKEY_BLOB(2, pubkey) &&
            BIND_H32_BLOB(3, hash) &&
            sqlite3_bind_int64(stmt, 4, blob_offset) == SQLITE_OK &&
            sqlite3_bind_int64(stmt, 5, output_count) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_DONE)
        {
            return 0;
        }

        LOG_ERROR << "Error inserting user output record.";
        return -1;
    }

    /**
     * Get the last ledger record of the given db.
     * @param db Pointer to the db.
     * @param ledger Ledger structure to populate.
     * @returns 0 on success. -1 on failure.
     */
    int get_last_ledger(sqlite3 *db, ledger::ledger_record &ledger)
    {
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, SELECT_LAST_LEDGER, -1, &stmt, 0) == SQLITE_OK && stmt != NULL &&
            sqlite3_step(stmt) == SQLITE_ROW)
        {
            populate_ledger_from_sql_record(ledger, stmt);
            sqlite3_finalize(stmt);
            return 0;
        }

        LOG_ERROR << "Error when querying last ledger from db. " << sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return -1;
    }

    /**
     * Get the ledger record by seq no.
     * @param db Pointer to the db.
     * @param seq_no Ledger sequence no. to search for.
     * @param ledger Ledger structure to populate.
     * @returns 1 if ledger found. 0 if ledger not found. -1 on failure.
     */
    int get_ledger_by_seq_no(sqlite3 *db, const uint64_t seq_no, ledger::ledger_record &ledger)
    {
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, SELECT_LEDGER_BY_SEQ_NO, -1, &stmt, 0) == SQLITE_OK && stmt != NULL &&
            sqlite3_bind_int64(stmt, 1, seq_no) == SQLITE_OK)
        {
            const int result = sqlite3_step(stmt);
            if (result == SQLITE_ROW)
            {
                populate_ledger_from_sql_record(ledger, stmt);
                sqlite3_finalize(stmt);
                return 1; // Ledger found.
            }
            else if (result == SQLITE_DONE)
            {
                sqlite3_finalize(stmt);
                return 0; // Not found.
            }
        }

        LOG_ERROR << "Error when querying ledger by seq no. from db. " << sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return -1;
    }

    int get_users_by_seq_no(sqlite3 *db, const uint64_t seq_no, std::vector<std::string> &users)
    {
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, SELECT_USERS_BY_SEQ_NO, -1, &stmt, 0) == SQLITE_OK && stmt != NULL &&
            sqlite3_bind_int64(stmt, 1, seq_no) == SQLITE_OK)
        {
            while (sqlite3_step(stmt) == SQLITE_ROW)
                users.push_back(GET_PUBKEY_BLOB(1));

            sqlite3_finalize(stmt);
            return 0;
        }

        LOG_ERROR << "Error when querying ledger users by seq no. from db. " << sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return -1;
    }

    int get_user_inputs_by_seq_no(sqlite3 *db, const uint64_t seq_no, std::vector<ledger::ledger_user_input> &inputs)
    {
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, SELECT_INPUTS_BY_SEQ_NO, -1, &stmt, 0) == SQLITE_OK && stmt != NULL &&
            sqlite3_bind_int64(stmt, 1, seq_no) == SQLITE_OK)
        {
            while (sqlite3_step(stmt) == SQLITE_ROW)
                inputs.push_back(populate_user_input_from_sql_record(stmt));

            sqlite3_finalize(stmt);
            return 0;
        }

        LOG_ERROR << "Error when querying ledger inputs by seq no. from db. " << sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return -1;
    }

    int get_user_outputs_by_seq_no(sqlite3 *db, const uint64_t seq_no, std::vector<ledger::ledger_user_output> &outputs)
    {
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, SELECT_OUTPUTS_BY_SEQ_NO, -1, &stmt, 0) == SQLITE_OK && stmt != NULL &&
            sqlite3_bind_int64(stmt, 1, seq_no) == SQLITE_OK)
        {
            while (sqlite3_step(stmt) == SQLITE_ROW)
                outputs.push_back(populate_user_output_from_sql_record(stmt));

            sqlite3_finalize(stmt);
            return 0;
        }

        LOG_ERROR << "Error when querying ledger outputs by seq no. from db. " << sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return -1;
    }

    int get_user_input_by_hash(sqlite3 *db, std::string_view hash, std::optional<ledger::ledger_user_input> &input)
    {
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, SELECT_INPUT_BY_HASH, -1, &stmt, 0) == SQLITE_OK && stmt != NULL &&
            BIND_H32_BLOB(1, hash))
        {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                input = populate_user_input_from_sql_record(stmt);

            sqlite3_finalize(stmt);
            return 0;
        }

        LOG_ERROR << "Error when querying ledger inputs by hash. from db. " << sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return -1;
    }

    void populate_ledger_from_sql_record(ledger::ledger_record &ledger, sqlite3_stmt *stmt)
    {
        ledger.seq_no = sqlite3_column_int64(stmt, 0);
        ledger.timestamp = sqlite3_column_int64(stmt, 1);
        ledger.ledger_hash = GET_H32_BLOB(2);
        ledger.prev_ledger_hash = GET_H32_BLOB(3);
        ledger.data_hash = GET_H32_BLOB(4);
        ledger.state_hash = GET_H32_BLOB(5);
        ledger.config_hash = GET_H32_BLOB(6);
        ledger.nonce = GET_H32_BLOB(7);
        ledger.user_hash = GET_H32_BLOB(8);
        ledger.input_hash = GET_H32_BLOB(9);
        ledger.output_hash = GET_H32_BLOB(10);
    }

    ledger::ledger_user_input populate_user_input_from_sql_record(sqlite3_stmt *stmt)
    {
        ledger::ledger_user_input inp;
        inp.ledger_seq_no = sqlite3_column_int64(stmt, 0);
        inp.pubkey = GET_PUBKEY_BLOB(1);
        inp.hash = GET_H32_BLOB(2);
        inp.nonce = sqlite3_column_int64(stmt, 3);
        inp.blob_offset = sqlite3_column_int64(stmt, 4);
        inp.blob_size = sqlite3_column_int64(stmt, 5);
        return inp;
    }

    ledger::ledger_user_output populate_user_output_from_sql_record(sqlite3_stmt *stmt)
    {
        ledger::ledger_user_output out;
        out.ledger_seq_no = sqlite3_column_int64(stmt, 0);
        out.pubkey = GET_PUBKEY_BLOB(1);
        out.hash = GET_H32_BLOB(2);
        out.blob_offset = sqlite3_column_int64(stmt, 3);
        out.blob_count = sqlite3_column_int64(stmt, 4);
        return out;
    }

} // namespace ledger::sqlite
