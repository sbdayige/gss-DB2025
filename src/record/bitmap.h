/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cinttypes>
#include <cstring>

static constexpr int BITMAP_WIDTH = 8;
static constexpr unsigned BITMAP_HIGHEST_BIT = 0x80u;  // 128 (2^7)

class Bitmap {
   public:
    // 从地址bm开始的size个字节全部置0
    static void init(char *bm, int size) { memset(bm, 0, size); }

    // pos位 置1
    static void set(char *bm, int pos) { bm[get_bucket(pos)] |= get_bit(pos); }

    // pos位 置0
    static void reset(char *bm, int pos) { bm[get_bucket(pos)] &= static_cast<char>(~get_bit(pos)); }

    // 如果pos位是1，则返回true
    static bool is_set(const char *bm, int pos) { return (bm[get_bucket(pos)] & get_bit(pos)) != 0; }

    /**
     * @brief 找下一个为0 or 1的位
     * @param bit false表示要找下一个为0的位，true表示要找下一个为1的位
     * @param bm 要找的起始地址为bm
     * @param max_n 要找的从起始地址开始的偏移为[curr+1,max_n)
     * @param curr 要找的从起始地址开始的偏移为[curr+1,max_n)
     * @return 找到了就返回偏移位置，没找到就返回max_n
     */
    static int next_bit(bool bit, const char *bm, int max_n, int curr) {
        for (int i = curr + 1; i < max_n; i++) {
            if (is_set(bm, i) == bit) {
                return i;
            }
        }
        return max_n;
    }

    // 找第一个为0 or 1的位
    static int first_bit(bool bit, const char *bm, int max_n) { return next_bit(bit, bm, max_n, -1); }

    // for example:
    // rid_.slot_no = Bitmap::next_bit(true, page_handle.bitmap, file_handle_->file_hdr_.num_records_per_page,
    // rid_.slot_no); int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    static char create_mask(int start_bit, int count) {
        char mask = 0;
        for (int i = 0; i < count && (start_bit + i) < BITMAP_WIDTH; i++) {
            mask |= (BITMAP_HIGHEST_BIT >> (start_bit + i));
        }
        return mask;
    }

    static void batch_reset_fast(char *bm, int start_pos, int count) {
        if (count <= 0) return;

        int end_pos = start_pos + count - 1;
        int start_bucket = get_bucket(start_pos);
        int end_bucket = get_bucket(end_pos);

        if (start_bucket == end_bucket) {
            // 在同一字节内，使用位掩码
            char mask = create_mask(start_pos % BITMAP_WIDTH, count);
            bm[start_bucket] &= static_cast<char>(~mask);  // 使用反掩码清零
        } else {
            // 跨多个字节

            // 1. 处理起始字节
            int start_offset = start_pos % BITMAP_WIDTH;
            if (start_offset != 0) {
                int bits_in_first_byte = BITMAP_WIDTH - start_offset;
                char mask = create_mask(start_offset, bits_in_first_byte);
                bm[start_bucket] &= static_cast<char>(~mask);  // 使用反掩码清零
                start_bucket++;
            }

            // 2. 批量处理完整字节（设置为0x00）
            memset(bm + start_bucket, 0x00, end_bucket - start_bucket);

            // 3. 处理结尾字节
            int end_offset = end_pos % BITMAP_WIDTH;
            if (end_bucket > start_bucket - 1) {
                char mask = create_mask(0, end_offset + 1);
                bm[end_bucket] &= static_cast<char>(~mask);  // 使用反掩码清零
            }
        }
    }

    static void batch_set_fast(char *bm, int start_pos, int count) {
        if (count <= 0) return;

        int end_pos = start_pos + count - 1;
        int start_bucket = get_bucket(start_pos);
        int end_bucket = get_bucket(end_pos);

        if (start_bucket == end_bucket) {
            // 在同一字节内，使用位掩码
            char mask = create_mask(start_pos % BITMAP_WIDTH, count);
            bm[start_bucket] |= mask;
        } else {
            // 跨多个字节

            // 1. 处理起始字节
            int start_offset = start_pos % BITMAP_WIDTH;
            if (start_offset != 0) {
                int bits_in_first_byte = BITMAP_WIDTH - start_offset;
                char mask = create_mask(start_offset, bits_in_first_byte);
                bm[start_bucket] |= mask;
                start_bucket++;
            }

            // 2. 批量处理完整字节
            memset(bm + start_bucket, 0xFF, end_bucket - start_bucket);

            // 3. 处理结尾字节
            int end_offset = end_pos % BITMAP_WIDTH;
            if (end_bucket > start_bucket - 1) {
                char mask = create_mask(0, end_offset + 1);
                bm[end_bucket] |= mask;
            }
        }
    }

   private:
    static int get_bucket(int pos) { return pos / BITMAP_WIDTH; }

    static char get_bit(int pos) { return BITMAP_HIGHEST_BIT >> static_cast<char>(pos % BITMAP_WIDTH); }
};
