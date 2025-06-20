#ifndef _HP_UTIL_BUFFER_STORE_
#define _HP_UTIL_BUFFER_STORE_

#include "../pchheader.hpp"

namespace util
{

    struct buffer_view
    {
        off_t offset;
        uint32_t size;

        bool is_null()
        {
            return !offset && !size;
        }
    };

    class buffer_store
    {
    private:
        off_t next_write_pos = 0;

    public:
        int fd;
        int init();
        const buffer_view write_buf(const void *buf, const uint32_t size);
        int read_buf(const buffer_view &view, std::string &buf);
        int purge(const buffer_view &buf);
        void deinit();
    };

} // namespace util

#endif