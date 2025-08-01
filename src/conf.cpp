#include "pchheader.hpp"
#include "conf.hpp"
#include "crypto.hpp"
#include "sc/sc.hpp"
#include "util/version.hpp"
#include "util/util.hpp"
#include "ledger/ledger_mount.hpp"
#include "sc/contract_mount.hpp"

namespace conf
{

    // Global contract context struct exposed to the application.
    contract_ctx ctx;

    // Global configuration struct exposed to the application.
    hp_config cfg;

    // Stores the initial startup role of the node.
    ROLE startup_role;

    constexpr int FILE_PERMS = 0644;
    constexpr uint32_t MAX_ROUND_TIME = 3600000;
    constexpr uint32_t MAX_STAGE_SLICE = 33;
    constexpr uint32_t MAX_THRESHOLD = 100;

    constexpr const char *ROLE_OBSERVER = "observer";
    constexpr const char *ROLE_VALIDATOR = "validator";
    constexpr const char *HISTORY_FULL = "full";
    constexpr const char *HISTORY_CUSTOM = "custom";
    constexpr const char *MODE_PUBLIC = "public";
    constexpr const char *MODE_PRIVATE = "private";

    bool init_success = false;

    /**
     * Loads and initializes the config for execution. Must be called once during application startup.
     * @return 0 for success. -1 for failure.
     */
    int init()
    {
        // The validations/loading needs to be in this order.
        // 1. Validate contract directories
        // 2. Read and load the config into memory
        // 4. Validate the loaded config values
        // 5. Initialize logging subsystem.
        // 6. Update and validate config if patch file exists.

        if (validate_contract_dir_paths() == -1 ||
            set_config_lock() == -1 ||
            read_config(cfg) == -1 ||
            validate_config(cfg) == -1)
        {
            release_config_lock();
            return -1;
        }

        init_success = true;
        return 0;
    }

    /**
     * Cleanup any resources.
     */
    void deinit()
    {
        if (init_success)
        {
            if (persist_updated_configs() == -1)
            {
                LOG_ERROR << "Failed to persist config updates.";
            }

            // Releases the config file lock at the termination.
            release_config_lock();
        }
    }

    /**
     * Generates and saves new signing keys in the config.
     */
    int rekey()
    {
        // Locking the config file at the startup. To check whether there's any already running hp instances.
        if (set_config_lock() == -1)
            return -1;

        // Load the config and re-save with the newly generated keys.
        hp_config cfg = {};
        if (read_config(cfg) != 0)
            return -1;

        // Old public key in binary format.
        const std::string old_pub_key_bin = cfg.node.public_key;

        crypto::generate_signing_keys(cfg.node.public_key, cfg.node.private_key);
        cfg.node.public_key_hex = util::to_hex(cfg.node.public_key);
        cfg.node.private_key_hex = util::to_hex(cfg.node.private_key);

        // Extract old pubkey from unl and re-insert the new public key in to the unl list.
        auto extracted_public_key_node = cfg.contract.unl.extract(old_pub_key_bin);
        if (extracted_public_key_node.empty())
        {
            std::cerr << "Updating public key in unl list failed.\n";
            return -1;
        }

        extracted_public_key_node.value() = cfg.node.public_key;
        cfg.contract.unl.insert(std::move(extracted_public_key_node));

        if (write_config(cfg) != 0)
            return -1;

        std::cout << "New signing keys generated at " << ctx.config_file << std::endl;

        // Releases the config file lock at the termination.
        release_config_lock();

        return 0;
    }

    /**
     * Creates a new contract directory with the default config.
     * By the time this gets called, the 'ctx' struct must be populated.
     * This function makes use of the paths populated in the ctx.
     */
    int create_contract()
    {
        if (util::is_dir_exists(ctx.contract_dir))
        {
            std::cerr << "Contract dir already exists. Cannot create contract at the same location.\n";
            return -1;
        }

        // Recursivly create contract directories. Return an error if unable to create
        if (util::create_dir_tree_recursive(ctx.config_dir) == -1 ||
            util::create_dir_tree_recursive(ctx.log_dir) == -1 ||
            util::create_dir_tree_recursive(ctx.contract_hpfs_dir + "/seed" + sc::STATE_DIR_PATH) == -1 ||
            util::create_dir_tree_recursive(ctx.contract_hpfs_mount_dir) == -1 ||
            util::create_dir_tree_recursive(ctx.ledger_hpfs_dir + "/seed" + ledger::PRIMARY_DIR) == -1 ||
            util::create_dir_tree_recursive(ctx.ledger_hpfs_dir + "/seed" + ledger::RAW_DIR) == -1 ||
            util::create_dir_tree_recursive(ctx.ledger_hpfs_mount_dir) == -1 ||
            util::create_dir_tree_recursive(ctx.contract_log_dir) == -1)
        {
            std::cerr << "ERROR: unable to create directories.\n";
            return -1;
        }

        // Create config file with default settings.

        // We populate the in-memory struct with default settings and then save it to the file.
        {
            hp_config cfg = {};

            crypto::generate_signing_keys(cfg.node.public_key, cfg.node.private_key);
            cfg.node.public_key_hex = util::to_hex(cfg.node.public_key);
            cfg.node.private_key_hex = util::to_hex(cfg.node.private_key);

            cfg.node.role = ROLE::VALIDATOR;
            cfg.node.history = HISTORY::CUSTOM;
            cfg.node.history_config.max_primary_shards = 1;
            cfg.node.history_config.max_raw_shards = 0;

            cfg.contract.id = crypto::generate_uuid();
            cfg.contract.execute = true;
            cfg.contract.log.enable = false;
            cfg.contract.log.max_mbytes_per_file = 5;
            cfg.contract.log.max_file_count = 10;
            cfg.contract.version = "1.0";
            // Add self pubkey to the unl.
            cfg.contract.unl.emplace(cfg.node.public_key);
            cfg.contract.bin_path = "<your contract binary here>";
            cfg.contract.consensus.roundtime = 1000;
            cfg.contract.consensus.stage_slice = 25;
            cfg.contract.consensus.mode = MODE::PRIVATE;
            cfg.contract.consensus.threshold = 80;
            cfg.contract.npl.mode = MODE::PRIVATE;
            cfg.contract.max_input_ledger_offset = 10;

            cfg.mesh.port = 22860;
            cfg.mesh.msg_forwarding = true;
            cfg.mesh.idle_timeout = 120000;
            cfg.mesh.peer_discovery.enabled = true;
            cfg.mesh.peer_discovery.interval = 30000;

            cfg.user.port = 8080;
            cfg.user.idle_timeout = 0;

            cfg.hpfs.log.log_level = "wrn";

            cfg.log.max_file_count = 10;
            cfg.log.max_mbytes_per_file = 5;
            cfg.log.log_level = "inf";
            cfg.log.loggers.emplace("console");
            cfg.log.loggers.emplace("file");

            // Save the default settings into the config file.
            if (write_config(cfg) != 0)
                return -1;
        }

        // Attempt to generate tls key files if openssl command is available.
        {
            const std::string tls_command = "openssl req -newkey rsa:2048 -new -nodes -x509 -days 365 -keyout " +
                                            ctx.config_dir + "/tlskey.pem" + " -out " + ctx.config_dir + "/tlscert.pem " +
                                            "-subj \"/C=HP/ST=HP/L=HP/O=HP/CN=" + cfg.node.public_key_hex + ".hotpocket.contract\" > /dev/null 2>&1";

            // We don't mind if this command fails, because when running the contract we'll check and inform the user that
            // tls key files are missing, so they can create them manually.
            if (system(tls_command.c_str()) == -1)
                std::cerr << errno << ": tls cert generation failed.";
        }

        std::cout << "Contract directory created at " << ctx.contract_dir << std::endl;

        return 0;
    }

    /**
     * Updates the contract context with directory paths based on provided base directory.
     * This is called after parsing HP command line arg in order to populate the ctx.
     */
    void set_contract_dir_paths(std::string exepath, std::string basedir)
    {
        // resolving the path through realpath will remove any trailing slash if present
        exepath = util::realpath(exepath);

        if (exepath.empty())
        {
            std::cerr << "Executable path must be specified\n";
            exit(1);
        }

        if (basedir.empty())
        {
            std::cerr << "a contract directory must be specified\n";
            exit(1);
        }

        // Take the parent directory path.
        ctx.exe_dir = dirname(exepath.data());

        ctx.hpws_exe_path = ctx.exe_dir + "/" + "hpws";
        ctx.hpfs_exe_path = ctx.exe_dir + "/" + "hpfs";

        ctx.contract_dir = basedir;
        ctx.config_dir = basedir + "/cfg";
        ctx.config_file = ctx.config_dir + "/hp.cfg";
        ctx.tls_key_file = ctx.config_dir + "/tlskey.pem";
        ctx.tls_cert_file = ctx.config_dir + "/tlscert.pem";
        ctx.contract_hpfs_dir = basedir + "/contract_fs";
        ctx.contract_hpfs_mount_dir = ctx.contract_hpfs_dir + "/mnt";
        ctx.contract_hpfs_rw_dir = ctx.contract_hpfs_mount_dir + "/rw";
        ctx.ledger_hpfs_dir = basedir + "/ledger_fs";
        ctx.ledger_hpfs_mount_dir = ctx.ledger_hpfs_dir + "/mnt";
        ctx.ledger_hpfs_rw_dir = ctx.ledger_hpfs_mount_dir + "/rw";
        ctx.log_dir = basedir + "/log";
        ctx.contract_log_dir = ctx.log_dir + "/contract";
    }

    /**
     * Reads the config file on disk and populates the in-memory 'cfg' struct.
     * @return 0 for successful loading of config. -1 for failure.
     */
    int read_config(hp_config &cfg)
    {
        // Read the config file into json document object.
        std::string buf;
        if (util::read_from_fd(ctx.config_fd, buf) == -1)
        {
            std::cerr << "Error reading from the config file. " << errno << '\n';
            return -1;
        }

        jsoncons::ojson d;
        try
        {
            d = jsoncons::ojson::parse(buf, jsoncons::strict_json_parsing());
        }
        catch (const std::exception &e)
        {
            std::cerr << "Invalid config file format. " << e.what() << '\n';
            return -1;
        }
        buf.clear();

        try
        {
            // Check whether the hp version is specified.
            const std::string config_version = d["hp_version"].as<std::string>();
            if (config_version.empty())
            {
                std::cerr << "Config HP version missing.\n";
                return -1;
            }

            // Check whether this config complies with the min version requirement.
            const int ver_result = version::version_compare(config_version, std::string(version::MIN_CONFIG_VERSION));
            if (ver_result == -1)
            {
                std::cerr << "Config version too old. Minimum " << version::MIN_CONFIG_VERSION << " required. "
                          << config_version << " found.\n";
                return -1;
            }
            else if (ver_result == -2)
            {
                std::cerr << "Malformed version string.\n";
                return -1;
            }

            // TODO: If our version and config version is different, we need to upgrade the contract dir.
        }
        catch (const std::exception &e)
        {
            std::cerr << "Required config field hp_version missing at " << ctx.config_file << std::endl;
            return -1;
        }

        std::string jpath;

        // node
        {
            jpath = "node";

            try
            {
                const jsoncons::ojson &node = d["node"];
                cfg.node.public_key_hex = node["public_key"].as<std::string>();
                cfg.node.private_key_hex = node["private_key"].as<std::string>();

                // Convert the hex keys to binary.
                cfg.node.public_key = util::to_bin(cfg.node.public_key_hex);
                if (cfg.node.public_key.empty())
                {
                    std::cerr << "Error decoding hex public key.\n";
                    return -1;
                }

                cfg.node.private_key = util::to_bin(cfg.node.private_key_hex);
                if (cfg.node.private_key.empty())
                {
                    std::cerr << "Error decoding hex private key.\n";
                    return -1;
                }

                if (node["role"] == ROLE_OBSERVER)
                    startup_role = cfg.node.role = ROLE::OBSERVER;
                else if (node["role"] == ROLE_VALIDATOR)
                    startup_role = cfg.node.role = ROLE::VALIDATOR;
                else
                {
                    std::cerr << "Invalid role. 'observer' or 'validator' expected.\n";
                    return -1;
                }

                if (node["history"] == HISTORY_FULL)
                    cfg.node.history = HISTORY::FULL;
                else if (node["history"] == HISTORY_CUSTOM)
                    cfg.node.history = HISTORY::CUSTOM;
                else
                {
                    std::cerr << "Invalid history mode. 'full' or 'custom' expected.\n";
                    return -1;
                }

                jpath = "node.history_config";
                cfg.node.history_config.max_primary_shards = node["history_config"]["max_primary_shards"].as<uint64_t>();
                cfg.node.history_config.max_raw_shards = node["history_config"]["max_raw_shards"].as<uint64_t>();

                // Max shards cannot be zero for primary and raw shards if the history mode is custom.
                // In history = full, these configs are not used.
                if (cfg.node.history == HISTORY::CUSTOM)
                {
                    if (cfg.node.history_config.max_primary_shards == 0)
                    {
                        std::cerr << "'max_primary_shards' cannot be zero in history=custom mode.\n";
                        return -1;
                    }
                }
            }
            catch (const std::exception &e)
            {
                print_missing_field_error(jpath, e);
                return -1;
            }
        }

        // contract
        {
            if (parse_contract_section_json(cfg.contract, d["contract"], false) == -1)
                return -1;
        }

        // mesh
        {
            jpath = "mesh";

            try
            {
                const jsoncons::ojson &mesh = d["mesh"];
                cfg.mesh.port = mesh["port"].as<uint16_t>();
                cfg.mesh.listen = mesh["listen"].as<bool>();
                cfg.mesh.idle_timeout = mesh["idle_timeout"].as<uint32_t>();

                // Storing peers in unordered map keyed by the concatenated address:port and also saving address and port
                // seperately to retrieve easily when handling peer connections.
                std::vector<std::string> splitted_peers;
                cfg.mesh.known_peers.clear();
                for (auto &v : mesh["known_peers"].array_range())
                {
                    peer_ip_port ipp;
                    std::string_view ipport_concat = v.as<std::string_view>();
                    if (ipp.from_string(ipport_concat) == -1)
                    {
                        std::cerr << "Invalid peer: " << ipport_concat << "\n";
                        return -1;
                    }
                    cfg.mesh.known_peers.emplace(ipp);
                    splitted_peers.clear();
                }
                cfg.mesh.msg_forwarding = mesh["msg_forwarding"].as<bool>();
                cfg.mesh.max_connections = mesh["max_connections"].as<uint16_t>();
                cfg.mesh.max_known_connections = mesh["max_known_connections"].as<uint16_t>();
                cfg.mesh.max_in_connections_per_host = mesh["max_in_connections_per_host"].as<uint16_t>();
                // If a value is specified for max_connections then max_known_connections value should not be 0(unlimited) and it should be less or equal than max_connections.
                if (cfg.mesh.max_connections > 0 && (cfg.mesh.max_known_connections > cfg.mesh.max_connections || cfg.mesh.max_known_connections == 0))
                {
                    std::cerr << "Invalid configuration values: mesh max_known_connections count should not exceed mesh max_connections." << '\n';
                    return -1;
                }
                cfg.mesh.max_bytes_per_msg = mesh["max_bytes_per_msg"].as<uint64_t>();
                cfg.mesh.max_bytes_per_min = mesh["max_bytes_per_min"].as<uint64_t>();
                cfg.mesh.max_bad_msgs_per_min = mesh["max_bad_msgs_per_min"].as<uint64_t>();
                cfg.mesh.max_bad_msgsigs_per_min = mesh["max_bad_msgsigs_per_min"].as<uint64_t>();
                cfg.mesh.max_dup_msgs_per_min = mesh["max_dup_msgs_per_min"].as<uint64_t>();

                jpath = "mesh.peer_discovery";
                cfg.mesh.peer_discovery.interval = mesh["peer_discovery"]["interval"].as<uint16_t>();
                cfg.mesh.peer_discovery.enabled = mesh["peer_discovery"]["enabled"].as<bool>();
            }
            catch (const std::exception &e)
            {
                print_missing_field_error(jpath, e);
                return -1;
            }
        }

        // user
        {
            jpath = "user";

            try
            {
                const jsoncons::ojson &user = d["user"];
                cfg.user.port = user["port"].as<uint16_t>();
                cfg.user.listen = user["listen"].as<bool>();
                cfg.user.idle_timeout = user["idle_timeout"].as<uint32_t>();
                cfg.user.max_connections = user["max_connections"].as<uint64_t>();
                cfg.user.max_in_connections_per_host = user["max_in_connections_per_host"].as<uint64_t>();
                cfg.user.max_bytes_per_msg = user["max_bytes_per_msg"].as<uint64_t>();
                cfg.user.max_bytes_per_min = user["max_bytes_per_min"].as<uint64_t>();
                cfg.user.max_bad_msgs_per_min = user["max_bad_msgs_per_min"].as<uint64_t>();
                cfg.user.concurrent_read_requests = user["concurrent_read_requests"].as<uint64_t>();
            }
            catch (const std::exception &e)
            {
                print_missing_field_error(jpath, e);
                return -1;
            }
        }

        // hpfs
        {
            jpath = "hpfs";

            try
            {
                const jsoncons::ojson &hpfs = d["hpfs"];
                cfg.hpfs.external = hpfs["external"].as<bool>();

                const jsoncons::ojson &hpfs_log = hpfs["log"];
                cfg.hpfs.log.log_level = hpfs_log["log_level"].as<std::string>();
            }
            catch (const std::exception &e)
            {
                print_missing_field_error(jpath, e);
                return -1;
            }
        }

        // log
        {
            jpath = "log";

            try
            {
                const jsoncons::ojson &log = d["log"];
                cfg.log.log_level = log["log_level"].as<std::string>();
                cfg.log.log_level_type = get_loglevel_type(cfg.log.log_level);

                cfg.log.max_mbytes_per_file = log["max_mbytes_per_file"].as<size_t>();
                cfg.log.max_file_count = log["max_file_count"].as<size_t>();
                cfg.log.loggers.clear();
                for (auto &v : log["loggers"].array_range())
                    cfg.log.loggers.emplace(v.as<std::string>());
            }
            catch (const std::exception &e)
            {
                print_missing_field_error(jpath, e);
                return -1;
            }
        }

        return 0;
    }

    /**
     * Saves the provided 'cfg' struct into the config file.
     * @return 0 for successful save. -1 for failure.
     */
    int write_config(const hp_config &cfg)
    {
        // Popualte json document with 'cfg' values.
        // ojson is used instead of json to preserve insertion order.
        jsoncons::ojson d;
        d.insert_or_assign("hp_version", version::HP_VERSION);

        // Node config.
        {
            jsoncons::ojson node_config;
            node_config.insert_or_assign("public_key", cfg.node.public_key_hex);
            node_config.insert_or_assign("private_key", cfg.node.private_key_hex);
            node_config.insert_or_assign("role", cfg.node.role == ROLE::OBSERVER ? ROLE_OBSERVER : ROLE_VALIDATOR);
            node_config.insert_or_assign("history", cfg.node.history == HISTORY::FULL ? HISTORY_FULL : HISTORY_CUSTOM);

            jsoncons::ojson history_config;
            history_config.insert_or_assign("max_primary_shards", cfg.node.history_config.max_primary_shards);
            history_config.insert_or_assign("max_raw_shards", cfg.node.history_config.max_raw_shards);
            node_config.insert_or_assign("history_config", history_config);

            d.insert_or_assign("node", node_config);
        }

        // Contract config section.
        {
            jsoncons::ojson contract;
            populate_contract_section_json(contract, cfg.contract, false);
            d.insert_or_assign("contract", contract);
        }

        // Mesh configs.
        {
            jsoncons::ojson mesh_config;
            mesh_config.insert_or_assign("port", cfg.mesh.port);
            mesh_config.insert_or_assign("listen", cfg.mesh.listen);
            mesh_config.insert_or_assign("idle_timeout", cfg.mesh.idle_timeout);

            jsoncons::ojson peers(jsoncons::json_array_arg);
            for (const auto &ipp : cfg.mesh.known_peers)
            {
                const std::string concat_str = std::string(ipp.host_address).append(":").append(std::to_string(ipp.port));
                peers.push_back(concat_str);
            }
            mesh_config.insert_or_assign("known_peers", peers);
            mesh_config.insert_or_assign("msg_forwarding", cfg.mesh.msg_forwarding);
            mesh_config.insert_or_assign("max_connections", cfg.mesh.max_connections);
            mesh_config.insert_or_assign("max_known_connections", cfg.mesh.max_known_connections);
            mesh_config.insert_or_assign("max_in_connections_per_host", cfg.mesh.max_in_connections_per_host);
            mesh_config.insert_or_assign("max_bytes_per_msg", cfg.mesh.max_bytes_per_msg);
            mesh_config.insert_or_assign("max_bytes_per_min", cfg.mesh.max_bytes_per_min);
            mesh_config.insert_or_assign("max_bad_msgs_per_min", cfg.mesh.max_bad_msgs_per_min);
            mesh_config.insert_or_assign("max_bad_msgsigs_per_min", cfg.mesh.max_bad_msgsigs_per_min);
            mesh_config.insert_or_assign("max_dup_msgs_per_min", cfg.mesh.max_dup_msgs_per_min);

            jsoncons::ojson peer_discovery_config;
            peer_discovery_config.insert_or_assign("enabled", cfg.mesh.peer_discovery.enabled);
            peer_discovery_config.insert_or_assign("interval", cfg.mesh.peer_discovery.interval);

            mesh_config.insert_or_assign("peer_discovery", peer_discovery_config);
            d.insert_or_assign("mesh", mesh_config);
        }

        // User configs.
        {
            jsoncons::ojson user_config;
            user_config.insert_or_assign("port", cfg.user.port);
            user_config.insert_or_assign("listen", cfg.user.listen);
            user_config.insert_or_assign("idle_timeout", cfg.user.idle_timeout);
            user_config.insert_or_assign("max_bytes_per_msg", cfg.user.max_bytes_per_msg);
            user_config.insert_or_assign("max_bytes_per_min", cfg.user.max_bytes_per_min);
            user_config.insert_or_assign("max_bad_msgs_per_min", cfg.user.max_bad_msgs_per_min);
            user_config.insert_or_assign("max_connections", cfg.user.max_connections);
            user_config.insert_or_assign("max_in_connections_per_host", cfg.user.max_in_connections_per_host);
            user_config.insert_or_assign("concurrent_read_requests", cfg.user.concurrent_read_requests);
            d.insert_or_assign("user", user_config);
        }

        // hpfs configs
        {
            jsoncons::ojson hpfs_config;
            hpfs_config.insert_or_assign("external", cfg.hpfs.external);

            jsoncons::ojson hpfs_log_config;
            hpfs_log_config.insert_or_assign("log_level", cfg.hpfs.log.log_level);

            hpfs_config.insert_or_assign("log", hpfs_log_config);
            d.insert_or_assign("hpfs", hpfs_config);
        }

        // Log configs.
        {
            jsoncons::ojson log_config;
            log_config.insert_or_assign("log_level", cfg.log.log_level);
            log_config.insert_or_assign("max_mbytes_per_file", cfg.log.max_mbytes_per_file);
            log_config.insert_or_assign("max_file_count", cfg.log.max_file_count);

            jsoncons::ojson loggers(jsoncons::json_array_arg);
            for (std::string_view logger : cfg.log.loggers)
            {
                loggers.push_back(logger);
            }
            log_config.insert_or_assign("loggers", loggers);
            d.insert_or_assign("log", log_config);
        }

        return write_json_file(ctx.config_file, d);
    }

    /**
     * Validates the 'cfg' struct for invalid values.
     *
     * @return 0 for successful validation. -1 for failure.
     */
    int validate_config(const hp_config &cfg)
    {
        // Check for non-empty signing keys.
        // We also check for key pair validity as well in the below code.
        if (cfg.node.public_key_hex.empty() || cfg.node.private_key_hex.empty())
        {
            std::cerr << "Signing keys missing. Run with 'rekey' to generate new keys.\n";
            return -1;
        }

        // Other required fields.

        bool fields_invalid = false;
        fields_invalid |= cfg.contract.consensus.roundtime == 0 && std::cerr << "Invalid value for roundtime.\n";
        fields_invalid |= cfg.contract.consensus.stage_slice == 0 && std::cerr << "Invalid value for stage slice.\n";
        fields_invalid |= cfg.contract.consensus.threshold == 0 && std::cerr << "Invalid value for threshold.\n";
        fields_invalid |= cfg.contract.unl.empty() && std::cerr << "Invalid value for unl. Unl list cannot be empty.\n";
        fields_invalid |= cfg.contract.id.empty() && std::cerr << "Invalid value for contract id.\n";
        fields_invalid |= cfg.mesh.port == 0 && std::cerr << "Invalid value for mesh port.\n";
        fields_invalid |= cfg.user.port == 0 && std::cerr << "Invalid value for user port.\n";
        fields_invalid |= cfg.log.log_level.empty() && std::cerr << "Invalid value for loglevel.\n";
        fields_invalid |= cfg.log.loggers.empty() && std::cerr << "Invalid value for loggers.\n";

        if (fields_invalid)
        {
            std::cerr << "Invalid configuration values at " << ctx.config_file << std::endl;
            return -1;
        }

        // User settings
        if (cfg.user.concurrent_read_requests > CONCURRENT_READ_REQUEST_MAX_LIMIT)
        {
            std::cerr << "User concurrent_read_requests cannot exceed " << CONCURRENT_READ_REQUEST_MAX_LIMIT << "\n";
            return -1;
        }

        // Log settings
        const std::unordered_set<std::string> valid_loglevels({"dbg", "inf", "wrn", "err"});
        if (valid_loglevels.count(cfg.log.log_level) != 1)
        {
            std::cerr << "Invalid loglevel configured. Valid values: dbg|inf|wrn|err\n";
            return -1;
        }
        else if (valid_loglevels.count(cfg.hpfs.log.log_level) != 1)
        {
            std::cerr << "Invalid hpfs loglevel configured. Valid values: dbg|inf|wrn|err\n";
            return -1;
        }

        const std::unordered_set<std::string> valid_loggers({"console", "file"});
        for (const std::string &logger : cfg.log.loggers)
        {
            if (valid_loggers.count(logger) != 1)
            {
                std::cerr << "Invalid logger. Valid values: console|file\n";
                return -1;
            }
        }

        // Sign and verify a sample message to ensure we have a matching signing key pair.
        const std::string msg = "hotpocket";
        const std::string sig = crypto::sign(msg, cfg.node.private_key);
        if (crypto::verify(msg, sig, cfg.node.public_key) != 0)
        {
            std::cerr << "Invalid signing keys. Run with 'rekey' to generate new keys.\n";
            return -1;
        }

        return 0;
    }

    /**
     * Checks for the existence of all contract sub directories.
     *
     * @return 0 for successful validation. -1 for failure.
     */
    int validate_contract_dir_paths()
    {
        const std::string paths[8] = {
            ctx.contract_dir,
            ctx.config_file,
            ctx.contract_hpfs_dir,
            ctx.ledger_hpfs_dir,
            ctx.tls_key_file,
            ctx.tls_cert_file,
            ctx.hpfs_exe_path,
            ctx.hpws_exe_path};

        for (const std::string &path : paths)
        {
            if (!util::is_file_exists(path) && !util::is_dir_exists(path))
            {
                if (path == ctx.tls_key_file || path == ctx.tls_cert_file)
                {
                    std::cerr << path << " does not exist. Please provide self-signed certificates. Can generate using command\n"
                              << "openssl req -newkey rsa:2048 -new -nodes -x509 -days 365 -keyout tlskey.pem -out tlscert.pem\n"
                              << "and add it to " + ctx.config_dir << std::endl;
                }
                else if (path == ctx.hpfs_exe_path || path == ctx.hpws_exe_path)
                {
                    std::cerr << path << " binary does not exist.\n";
                }
                else
                {
                    std::cerr << path << " does not exist.\n";
                }

                return -1;
            }
        }

        return 0;
    }

    void change_role(const ROLE role)
    {
        // Do not allow to change the mode if the node was started as an observer.
        if (startup_role == ROLE::OBSERVER || cfg.node.role == role)
            return;

        cfg.node.role = role;

        if (role == ROLE::OBSERVER)
            LOG_INFO << "Switched to OBSERVER mode.";
        else
            LOG_INFO << "Switched back to VALIDATOR mode.";
    }

    /**
     * Convert string to Log Severity enum type.
     * @param severity log severity code.
     * @return log severity type.
     */
    LOG_SEVERITY get_loglevel_type(std::string_view severity)
    {
        if (severity == "dbg")
            return LOG_SEVERITY::DEBUG;
        else if (severity == "wrn")
            return LOG_SEVERITY::WARN;
        else if (severity == "inf")
            return LOG_SEVERITY::INFO;
        else
            return LOG_SEVERITY::ERROR;
    }

    /**
     * Prints the config json parsing field missing error.
     */
    void print_missing_field_error(std::string_view jpath, const std::exception &e, const bool is_patch_config)
    {
        // Extract field name from jsoncons exception message.
        if (is_patch_config)
            LOG_ERROR << "Config validation error: " << e.what() << " in '" << jpath << "' section in patch config";
        else
            std::cerr << "Config validation error: " << e.what() << " in '" << jpath << "' section at " << ctx.config_file << std::endl;
    }

    /**
     * Populate patch.cfg in hpfs using current values in config.
     * @return Returns -1 on error and 0 on successful update.
     */
    int populate_patch_config()
    {
        jsoncons::ojson jdoc;
        populate_contract_section_json(jdoc, cfg.contract, true);

        const std::string patch_file_path = sc::contract_fs.physical_path(hpfs::RW_SESSION_NAME, sc::PATCH_FILE_PATH);
        return write_json_file(patch_file_path, jdoc);
    }

    /**
     * Validate and update config section if a patch file detected. Whenever patch file change is detected,
     * we also persist it to hp.cfg so that both config files are consistent with each other.
     * @param hpfs_session_name The current hpfs session hosting the patch config.
     * @return -1 on error and 0 in successful update.
     */
    int apply_patch_config(std::string_view hpfs_session_name)
    {
        const std::string path = sc::contract_fs.physical_path(hpfs_session_name, sc::PATCH_FILE_PATH);
        if (!util::is_file_exists(path))
            return 0;

        const int fd = open(path.data(), O_RDONLY);
        if (fd == -1)
        {
            std::cerr << "Error opening the patch config file. " << errno << '\n';
            return -1;
        }

        // If patch file exist, read the patch file values to a json doc and then persist the values into hp.cfg.
        std::string buf;
        if (util::read_from_fd(fd, buf) == -1)
        {
            std::cerr << "Error reading from the patch config file. " << errno << '\n';
            close(fd);
            return -1;
        }
        close(fd);

        jsoncons::ojson jdoc;
        try
        {
            jdoc = jsoncons::ojson::parse(buf, jsoncons::strict_json_parsing());
        }
        catch (const std::exception &e)
        {
            std::cerr << "Invalid patch config file format. " << e.what() << '\n';
            return -1;
        }
        buf.clear();

        // Persist new changes to config file and runtime config.
        hp_config temp_cfg;
        if (read_config(temp_cfg) == -1 ||
            parse_contract_section_json(temp_cfg.contract, jdoc, true) == -1 ||
            parse_contract_section_json(cfg.contract, jdoc, true) == -1 ||
            write_config(temp_cfg) == -1)
        {
            LOG_ERROR << "Error applying patch config.";
            return -1;
        }

        LOG_INFO << "Contract config updated from patch file.";
        return 0;
    }

    /**
     * Persists any updated config fields back to config file.
     */
    int persist_updated_configs()
    {
        bool changes_made = false;

        // Read the original config into a temp struct.
        hp_config temp_cfg;
        if (read_config(temp_cfg) == -1)
            return -1;

        // Apply any actual changes to the temp struct.

        // Apply known peer list updates.
        if (!cfg.mesh.known_peers.empty())
        {
            temp_cfg.mesh.known_peers = cfg.mesh.known_peers;
            changes_made = true;
        }

        // Persis the temp struct if any changes made to values.
        return changes_made ? write_config(temp_cfg) : 0;
    }

    /**
     * Locks the config file. If already locked means there's another hpcore instance running in the same directory.
     * If so, log error and return, Otherwise lock the config.
     * @return Returns 0 if lock is successfully acquired, -1 on error.
     */
    int set_config_lock()
    {
        ctx.config_fd = open(ctx.config_file.data(), O_RDWR, 444);
        if (ctx.config_fd == -1)
            return -1;

        if (util::set_lock(ctx.config_fd, ctx.config_lock, true, 0, 0) == -1)
        {
            if (errno == EACCES || errno == EAGAIN)
            {
                std::cerr << "Another hpcore instance is already running in directory " << ctx.contract_dir << "\n";
            }
            // Close fd if lock aquiring failed.
            close(ctx.config_fd);
            return -1;
        }

        return 0;
    }

    /**
     * Releases the config file and closes the opened file descriptor.
     * @return Returns 0 if lock is successfully acquired, -1 on error.
     */
    int release_config_lock()
    {
        const int res = util::release_lock(ctx.config_fd, ctx.config_lock);
        // Close fd in termination.
        close(ctx.config_fd);
        return res;
    }

    /**
     * Populates contract section field values into the provided json doc.
     * @param jdoc The json doc to populate contract section field values.
     * @param contract The contract fields struct containing current field values.
     * @param is_patch_config Whether this is called for patch config or not.
     */
    void populate_contract_section_json(jsoncons::ojson &jdoc, const contract_config &contract, const bool is_patch_config)
    {
        if (!is_patch_config)
        {
            jdoc.insert_or_assign("id", contract.id);
            jdoc.insert_or_assign("execute", contract.execute);
            jdoc.insert_or_assign("run_as", contract.run_as.to_string());
            jsoncons::ojson log;
            log.insert_or_assign("enable", contract.log.enable);
            log.insert_or_assign("max_mbytes_per_file", contract.log.max_mbytes_per_file);
            log.insert_or_assign("max_file_count", contract.log.max_file_count);
            jdoc.insert_or_assign("log", log);
        }

        jdoc.insert_or_assign("version", contract.version);
        jsoncons::ojson unl(jsoncons::json_array_arg);
        for (const auto &nodepk : contract.unl)
        {
            unl.push_back(util::to_hex(nodepk));
        }
        jdoc.insert_or_assign("unl", unl);
        jdoc.insert_or_assign("bin_path", contract.bin_path);
        jdoc.insert_or_assign("bin_args", contract.bin_args);
        jdoc.insert_or_assign("environment", contract.environment);
        jdoc.insert_or_assign("max_input_ledger_offset", contract.max_input_ledger_offset);

        jsoncons::ojson consensus;
        consensus.insert_or_assign("mode", contract.consensus.mode == MODE::PUBLIC ? MODE_PUBLIC : MODE_PRIVATE);
        consensus.insert_or_assign("roundtime", contract.consensus.roundtime.load());
        consensus.insert_or_assign("stage_slice", contract.consensus.stage_slice.load());
        consensus.insert_or_assign("threshold", contract.consensus.threshold);
        jdoc.insert_or_assign("consensus", consensus);

        jsoncons::ojson npl;
        npl.insert_or_assign("mode", contract.npl.mode == MODE::PUBLIC ? MODE_PUBLIC : MODE_PRIVATE);
        jdoc.insert_or_assign("npl", npl);

        jsoncons::ojson round_limits;
        round_limits.insert_or_assign("user_input_bytes", contract.round_limits.user_input_bytes);
        round_limits.insert_or_assign("user_output_bytes", contract.round_limits.user_output_bytes);
        round_limits.insert_or_assign("npl_output_bytes", contract.round_limits.npl_output_bytes);
        round_limits.insert_or_assign("proc_cpu_seconds", contract.round_limits.proc_cpu_seconds);
        round_limits.insert_or_assign("proc_mem_bytes", contract.round_limits.proc_mem_bytes);
        round_limits.insert_or_assign("proc_ofd_count", contract.round_limits.proc_ofd_count);
        round_limits.insert_or_assign("exec_timeout", contract.round_limits.exec_timeout);
        jdoc.insert_or_assign("round_limits", round_limits);
    }

    /**
     * Validates the provided json doc and populate the provided contract struct with values from json doc.
     * @param contract The contract fields struct to populate.
     * @param jdoc The json doc containing the contract section field values.
     * @param is_patch_config Whether this is called for patch config or not.
     * @return 0 on success. -1 on error.
     */
    int parse_contract_section_json(contract_config &contract, const jsoncons::ojson &jdoc, const bool is_patch_config)
    {
        std::string jpath = "contract";

        try
        {
            if (!is_patch_config)
            {
                contract.id = jdoc["id"].as<std::string>();
                if (contract.id.empty())
                {
                    std::cerr << "Contract id not specified.\n";
                    return -1;
                }

                contract.execute = jdoc["execute"].as<bool>();
                if (contract.run_as.from_string(jdoc["run_as"].as<std::string>()) == -1)
                {
                    std::cerr << "Invalid format for contract run as config (\"uid>0:gid>0\" expected).\n";
                    return -1;
                }

                jpath = "contract.log";
                contract.log.enable = jdoc["log"]["enable"].as<bool>();
                contract.log.max_mbytes_per_file = jdoc["log"]["max_mbytes_per_file"].as<size_t>();
                contract.log.max_file_count = jdoc["log"]["max_file_count"].as<size_t>();
                if (contract.log.enable)
                {
                    if (contract.log.max_mbytes_per_file <= 0)
                    {
                        std::cerr << "Contract log max mbytes per file must be greater than 0 to enable contract logging.\n";
                        return -1;
                    }
                    if (contract.log.max_file_count <= 0)
                    {
                        std::cerr << "Contract log file count must be greater than 0 to enable contract logging.\n";
                        return -1;
                    }
                }
                jpath = "contract";
            }

            contract.version = jdoc["version"].as<std::string>();
            if (contract.version.empty())
            {
                std::cerr << "Contract version not specified.\n";
                return -1;
            }

            contract.bin_path = jdoc["bin_path"].as<std::string>();
            contract.bin_args = jdoc["bin_args"].as<std::string>();
            contract.max_input_ledger_offset = jdoc["max_input_ledger_offset"].as<uint16_t>();

            jpath = "contract.environment";
            contract.environment.clear();
            for (const auto &obj : jdoc["environment"].object_range())
            {
                // Environment variable values should be strings.
                if (!obj.value().is_string())
                {
                    std::cerr << jpath << "." << obj.key() << " environment variable should be a string.\n";
                    return -1;
                }

                contract.environment.emplace(obj.key(), obj.value().as<std::string>());
            }

            jpath = "contract.unl";
            contract.unl.clear();
            for (auto &nodepk : jdoc["unl"].array_range())
            {
                // Convert the public key hex of each node to binary and store it.
                const std::string bin_pubkey = util::to_bin(nodepk.as<std::string_view>());
                if (bin_pubkey.empty())
                {
                    std::cerr << "Error decoding unl list.\n";
                    return -1;
                }
                contract.unl.emplace(bin_pubkey);
            }
            if (contract.unl.empty())
            {
                std::cerr << "UNL cannot be empty.\n";
                return -1;
            }

            jpath = "contract.consensus";
            contract.consensus.roundtime = jdoc["consensus"]["roundtime"].as<uint32_t>();
            if (contract.consensus.roundtime < 1 || contract.consensus.roundtime > MAX_ROUND_TIME)
            {
                std::cerr << "Round time must be between 1 and " << MAX_ROUND_TIME << "ms inclusive.\n";
                return -1;
            }

            contract.consensus.stage_slice = jdoc["consensus"]["stage_slice"].as<uint32_t>();
            if (contract.consensus.stage_slice < 1 || contract.consensus.stage_slice > MAX_STAGE_SLICE)
            {
                std::cerr << "Stage slice must be between 1 and " << MAX_STAGE_SLICE << " percent inclusive.\n";
                return -1;
            }

            contract.consensus.threshold = jdoc["consensus"]["threshold"].as<uint32_t>();
            if (contract.consensus.threshold < 1 || contract.consensus.threshold > MAX_THRESHOLD)
            {
                std::cerr << "Threshold must be between 1 and " << MAX_THRESHOLD << " percent inclusive.\n";
                return -1;
            }

            if (jdoc["consensus"]["mode"].as<std::string>() != MODE_PUBLIC && jdoc["consensus"]["mode"].as<std::string>() != MODE_PRIVATE)
            {
                std::cerr << "Invalid consensus flag configured. Valid values: public|private\n";
                return -1;
            }
            contract.consensus.mode = jdoc["consensus"]["mode"].as<std::string>() == MODE_PUBLIC ? MODE::PUBLIC : MODE::PRIVATE;

            jpath = "contract.npl";
            if (jdoc["npl"]["mode"].as<std::string>() != MODE_PUBLIC && jdoc["npl"]["mode"].as<std::string>() != MODE_PRIVATE)
            {
                std::cerr << "Invalid npl flag configured. Valid values: public|private\n";
                return -1;
            }
            contract.npl.mode = jdoc["npl"]["mode"].as<std::string>() == MODE_PUBLIC ? MODE::PUBLIC : MODE::PRIVATE;

            jpath = "contract.round_limits";
            contract.round_limits.user_input_bytes = jdoc["round_limits"]["user_input_bytes"].as<size_t>();
            contract.round_limits.user_output_bytes = jdoc["round_limits"]["user_output_bytes"].as<size_t>();
            contract.round_limits.npl_output_bytes = jdoc["round_limits"]["npl_output_bytes"].as<size_t>();
            contract.round_limits.proc_cpu_seconds = jdoc["round_limits"]["proc_cpu_seconds"].as<size_t>();
            contract.round_limits.proc_mem_bytes = jdoc["round_limits"]["proc_mem_bytes"].as<size_t>();
            contract.round_limits.proc_ofd_count = jdoc["round_limits"]["proc_ofd_count"].as<size_t>();
            contract.round_limits.exec_timeout = jdoc["round_limits"]["exec_timeout"].as<uint64_t>();
        }
        catch (const std::exception &e)
        {
            print_missing_field_error(jpath, e, is_patch_config);
            return -1;
        }

        contract.runtime_env_args.clear();
        // Populate runtime environment args.
        for (const auto &[key, val] : contract.environment)
            contract.runtime_env_args.push_back(key + "=" + val);

        contract.runtime_binexec_args.clear();
        // Populate runtime contract execution args.
        if (!contract.bin_args.empty())
            util::split_string(contract.runtime_binexec_args, contract.bin_args, " ");
        contract.runtime_binexec_args.insert(contract.runtime_binexec_args.begin(), contract.bin_path);

        // Uncomment for docker-based execution.
        // std::string volumearg;
        // volumearg.append("type=bind,source=").append(ctx.contract_hpfs_dir).append(",target=/hpfs");
        // const char *dockerargs[] = {"/usr/bin/docker", "run", "--rm", "-i", "--mount", volumearg.data(), contract.bin_path.data()};
        // contract.runtime_binexec_args.insert(contract.runtime_binexec_args.begin(), std::begin(dockerargs), std::end(dockerargs));

        return 0;
    }

    /**
     * Writes the given json doc to a file.
     * @return 0 on success. -1 on failure.
     */
    int write_json_file(const std::string &file_path, const jsoncons::ojson &d)
    {
        std::string json;
        // Convert json object to a string.
        try
        {
            jsoncons::json_options options;
            options.object_array_line_splits(jsoncons::line_split_kind::multi_line);
            options.spaces_around_comma(jsoncons::spaces_option::no_spaces);
            std::ostringstream os;
            os << jsoncons::pretty_print(d, options);
            json = os.str();
            os.clear();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Converting json to string failed. " << file_path << std::endl;
            return -1;
        }

        // O_TRUNC flag is used to trucate existing content from the file.
        const int fd = open(file_path.data(), O_CREAT | O_RDWR | O_TRUNC, FILE_PERMS);
        if (fd == -1 || write(fd, json.data(), json.size()) == -1)
        {
            std::cerr << "Writing file failed. " << file_path << std::endl;
            if (fd != -1)
                close(fd);
            return -1;
        }
        close(fd);
        return 0;
    }

} // namespace conf
