/*
 * Copyright(c) 2019 Netflix, Inc.
 * SPDX - License - Identifier: BSD - 2 - Clause - Patent
 */

/******************************************************************************
 * @file CdefTest.cc
 *
 * @brief Unit test for cdef tools:
 * * eb_cdef_find_dir_avx2
 * * eb_cdef_filter_block_avx2
 * * compute_cdef_dist_avx2
 * * copy_rect8_8bit_to_16bit_avx2
 * * search_one_dual_avx2
 *
 * @author Cidana-Wenyao
 *
 ******************************************************************************/
#include <cstdlib>
#include <string>
#include "gtest/gtest.h"
// workaround to eliminate the compiling warning on linux
// The macro will conflict with definition in gtest.h
#ifdef __USE_GNU
#undef __USE_GNU  // defined in EbThreads.h
#endif
#ifdef _GNU_SOURCE
#undef _GNU_SOURCE  // defined in EbThreads.h
#endif
#include "EbDefinitions.h"
#include "aom_dsp_rtcd.h"
#include "EbCdef.h"
#include "util.h"
#include "random.h"
#include "EbUtility.h"

using ::testing::make_tuple;
using svt_av1_test_tool::SVTRandom;
namespace {

using cdef_dir_param_t =
    ::testing::tuple<cdef_filter_block_func, cdef_filter_block_func, BlockSize,
                     int, int>;

/**
 * @brief Unit test for eb_cdef_filter_block_avx2
 *
 * Test strategy:
 * Feed src data generated randomly and all possible input,
 * then check the dst buffer from target function and reference
 * function.
 *
 * Expect result:
 * The dst buffer from targeted function
 * should be identical with the values from reference function.
 *
 * Test coverage:
 * Test cases:
 * primary_strength: [0, 15] << (bd_ - 8)
 * second_strength: 0, 1, 2, 4
 * primary_damping: [3, 6] + (bd_ - 8)
 * second_damping: [3, 6] + (bd_ - 8)
 * direction: [0, 7]
 * bitdepth: 8, 10, 12
 *
 */
class CDEFBlockTest : public ::testing::TestWithParam<cdef_dir_param_t> {
  public:
    CDEFBlockTest() : rnd_(0, (1 << 16) - 1) {
    }

    virtual ~CDEFBlockTest() {
    }

    virtual void SetUp() {
        cdef_tst_ = TEST_GET_PARAM(0);
        cdef_ref_ = TEST_GET_PARAM(1);
        bsize_ = TEST_GET_PARAM(2);
        boundary_ = TEST_GET_PARAM(3);
        bd_ = TEST_GET_PARAM(4);

        memset(dst_ref_, 0, sizeof(dst_ref_));
        memset(dst_tst_, 0, sizeof(dst_tst_));
    }

    virtual void TearDown() {
        aom_clear_system_state();
    }

    void prepare_data(int level, int bits) {
        for (uint32_t i = 0; i < sizeof(src_) / sizeof(*src_); i++)
            src_[i] = clamp(
                (rnd_.random() & ((1 << bits) - 1)) + level, 0, (1 << bd_) - 1);

        if (boundary_) {
            if (boundary_ & 1) {  // Left
                for (int i = 0; i < ysize_; i++)
                    for (int j = 0; j < CDEF_HBORDER; j++)
                        src_[i * CDEF_BSTRIDE + j] = CDEF_VERY_LARGE;
            }
            if (boundary_ & 2) {  // Right
                for (int i = 0; i < ysize_; i++)
                    for (int j = CDEF_HBORDER + size_; j < CDEF_BSTRIDE; j++)
                        src_[i * CDEF_BSTRIDE + j] = CDEF_VERY_LARGE;
            }
            if (boundary_ & 4) {  // Above
                for (int i = 0; i < CDEF_VBORDER; i++)
                    for (int j = 0; j < CDEF_BSTRIDE; j++)
                        src_[i * CDEF_BSTRIDE + j] = CDEF_VERY_LARGE;
            }
            if (boundary_ & 8) {  // Below
                for (int i = CDEF_VBORDER + size_; i < ysize_; i++)
                    for (int j = 0; j < CDEF_BSTRIDE; j++)
                        src_[i * CDEF_BSTRIDE + j] = CDEF_VERY_LARGE;
            }
        }
    }

    void run_test(int pri_damping, int sec_damping) {
        int pri_strength, sec_strength;
        int dir;
        unsigned int pos = 0;
        const unsigned int max_pos =
            size_ * size_ >> static_cast<int>(bd_ == 8);
        for (dir = 0; dir < 8; dir++) {
            // primary strength range between [0, 15], scale the range and step
            // for high bitdepth; For example, 12-bit content can have strengths
            // value of 0, 16, 32
            for (pri_strength = 0; pri_strength <= 19 << (bd_ - 8);
                 pri_strength += (1 + 4 * !!boundary_) << (bd_ - 8)) {
                if (pri_strength == 16)
                    pri_strength = 19;
                /* second strength can only be 0, 1, 2, 4 for 8-bit */
                for (sec_strength = 0; sec_strength <= 4 << (bd_ - 8);
                     sec_strength += 1 << (bd_ - 8)) {
                    if (sec_strength == 3 << (bd_ - 8))
                        continue;
                    cdef_ref_(bd_ == 8 ? (uint8_t *)dst_ref_ : 0,
                              dst_ref_,
                              size_,
                              src_ + CDEF_HBORDER + CDEF_VBORDER * CDEF_BSTRIDE,
                              pri_strength,
                              sec_strength,
                              dir,
                              pri_damping,
                              sec_damping,
                              bsize_,
                              0 /*Not used*/,
                              bd_ - 8);
                    cdef_tst_(bd_ == 8 ? (uint8_t *)dst_tst_ : 0,
                              dst_tst_,
                              size_,
                              src_ + CDEF_HBORDER + CDEF_VBORDER * CDEF_BSTRIDE,
                              pri_strength,
                              sec_strength,
                              dir,
                              pri_damping,
                              sec_damping,
                              bsize_,
                              0 /*not used*/,
                              bd_ - 8);

                    for (pos = 0; pos < max_pos; pos++) {
                        ASSERT_EQ(dst_ref_[pos], dst_tst_[pos])
                            << "Error: CDEFBlockTest, SIMD and C mismatch."
                            << std::endl
                            << "First error at " << pos % size_ << ","
                            << pos / size_ << " (" << (int16_t)dst_ref_[pos]
                            << " : " << (int16_t)dst_tst_[pos] << ") "
                            << std::endl
                            << "pristrength: " << pri_strength << std::endl
                            << "pridamping: " << pri_damping << std::endl
                            << "secstrength: " << sec_strength << std::endl
                            << "secdamping: " << sec_damping << std::endl
                            << "bitdepth: " << bd_ << std::endl
                            << "size: " << bsize_ << std::endl
                            << "boundary: " << boundary_ << std::endl
                            << std::endl;
                    }
                }
            }
        }
    }

    void test_cdef(int iterations) {
        int pri_damping, sec_damping, bits, level, count;
        // for 8-bit content, damping ranges [3, 6] for luma, [2, 5] for chroma
        // for highbd content, range will add the additinal bitdepth. 12-bit
        // content will range [7, 10]
        const int scale = boundary_ > 0 ? 1 : 0;
        const int min_damping = 3 + bd_ - 8;
        const int max_damping = 7 - 3 * scale + bd_ - 8;
        for (pri_damping = min_damping; pri_damping < max_damping;
             pri_damping++) {
            for (sec_damping = min_damping; sec_damping < max_damping;
                 sec_damping++) {
                for (count = 0; count < iterations; count++) {
                    for (level = 0; level < (1 << bd_);
                         level += (2 + 6 * scale) << (bd_ - 8)) {
                        for (bits = 1; bits <= bd_; bits += 1 + 3 * scale) {
                            prepare_data(level, bits);
                            run_test(pri_damping, sec_damping);
                        }
                    }
                }
            }
        }
    }

  protected:
    int bsize_;
    int boundary_;
    int bd_;
    cdef_filter_block_func cdef_tst_;
    cdef_filter_block_func cdef_ref_;
    SVTRandom rnd_;
    static const int size_ = 8;
    static const int ysize_ = size_ + 2 * CDEF_VBORDER;
    DECLARE_ALIGNED(16, uint16_t, src_[ysize_ * CDEF_BSTRIDE]);
    DECLARE_ALIGNED(16, uint16_t, dst_tst_[size_ * size_]);
    DECLARE_ALIGNED(16, uint16_t, dst_ref_[size_ * size_]);
};

TEST_P(CDEFBlockTest, MatchTest) {
    test_cdef(1);
}

// VS compiling for 32 bit targets does not support vector types in
// structs as arguments, which makes the v256 type of the intrinsics
// hard to support, so optimizations for this target are disabled.
#if defined(_WIN64) || !defined(_MSC_VER) || defined(__clang__)

INSTANTIATE_TEST_CASE_P(
    Cdef, CDEFBlockTest,
    ::testing::Combine(::testing::Values(&eb_cdef_filter_block_avx2),
                       ::testing::Values(&eb_cdef_filter_block_c),
                       ::testing::Values(BLOCK_4X4, BLOCK_4X8, BLOCK_8X4,
                                         BLOCK_8X8),
                       ::testing::Range(0, 16), ::testing::Range(8, 13, 2)));

#endif  // defined(_WIN64) || !defined(_MSC_VER)

using FindDirFunc = int (*)(const uint16_t *img, int stride, int32_t *var,
                            int coeff_shift);
using TestFindDirParam = ::testing::tuple<FindDirFunc, FindDirFunc>;

/**
 * @brief Unit test for eb_cdef_find_dir_avx2
 *
 * Test strategy:
 * Feed src data generated randomly, and check the best mse and
 * the directional contrast from target function and reference
 * function
 *
 * Expect result:
 * The best mse and directional contrast from targeted function
 * should be identical with the values from reference function.
 *
 * Test coverage:
 * Test cases:
 * bitdepth: 8, 10, 12
 *
 */
class CDEFFindDirTest : public ::testing::TestWithParam<TestFindDirParam> {
  public:
    CDEFFindDirTest() : rnd_(0, (1 << 16) - 1) {
    }

    virtual ~CDEFFindDirTest() {
    }

    virtual void SetUp() {
        func_tst_ = TEST_GET_PARAM(0);
        func_ref_ = TEST_GET_PARAM(1);
    }

    virtual void TearDown() {
        aom_clear_system_state();
    }

    void prepare_data(int depth, int bits, int level) {
        for (unsigned int i = 0; i < sizeof(src_) / sizeof(src_[0]); i++)
            src_[i] = clamp((rnd_.random() & ((1 << bits) - 1)) + level,
                            0,
                            (1 << depth) - 1);
    }

    void test_finddir() {
        int depth, bits, level, count;
        int res_ref = 0, res_tst = 0;
        int32_t var_ref = 0, var_tst = 0;

        for (depth = 8; depth <= 12; depth += 2) {
            for (count = 0; count < 512; count++) {
                const int shift = depth - 8;
                for (level = 0; level < (1 << depth); level += 1 << shift) {
                    for (bits = 1; bits <= depth; bits++) {
                        prepare_data(depth, bits, level);

                        res_ref = func_ref_(src_, size_, &var_ref, shift);
                        res_tst = func_tst_(src_, size_, &var_tst, shift);
                        ASSERT_EQ(res_tst, res_ref)
                            << "Error: CDEFFindDirTest, SIMD and C mismatch."
                            << "return " << res_tst << " : " << res_ref
                            << "depth: " << depth;
                        ASSERT_EQ(var_tst, var_ref)
                            << "Error: CDEFFindDirTest, SIMD and C mismatch."
                            << "var: " << var_tst << " : " << var_ref
                            << "depth: " << depth;
                    }
                }
            }
        }
    }

  protected:
    FindDirFunc func_tst_;
    FindDirFunc func_ref_;
    SVTRandom rnd_;
    static const int size_ = 8;
    DECLARE_ALIGNED(16, uint16_t, src_[size_ * size_]);
};

TEST_P(CDEFFindDirTest, MatchTest) {
    test_finddir();
}

// VS compiling for 32 bit targets does not support vector types in
// structs as arguments, which makes the v256 type of the intrinsics
// hard to support, so optimizations for this target are disabled.
#if defined(_WIN64) || !defined(_MSC_VER) || defined(__clang__)

INSTANTIATE_TEST_CASE_P(Cdef, CDEFFindDirTest,
                        ::testing::Values(make_tuple(&eb_cdef_find_dir_avx2,
                                                     &eb_cdef_find_dir_c)));

#endif  // defined(_WIN64) || !defined(_MSC_VER)
}  // namespace

/**
 * @brief Unit test for copy_rect8_8bit_to_16bit_avx2
 *
 * Test strategy:
 * Feed src data generated randomly, and check the dst data
 *
 * Expect result:
 * The dst data from targeted function should be identical
 * with the date from reference function.
 *
 * Test coverage:
 * Test cases:
 * hsize: [8, 64], step is 8
 * vsize: [8, 64], step is 8
 *
 */
TEST(CdefToolTest, CopyRectMatchTest) {
    SVTRandom rnd_(8, false);

    DECLARE_ALIGNED(16, uint8_t, src_data_[CDEF_INBUF_SIZE]);
    DECLARE_ALIGNED(16, uint16_t, dst_data_tst_[CDEF_INBUF_SIZE]);
    DECLARE_ALIGNED(16, uint16_t, dst_data_ref_[CDEF_INBUF_SIZE]);

    // prepare src data
    for (int i = 0; i < CDEF_INBUF_SIZE; ++i)
        src_data_[i] = rnd_.random();

    // assume the width or height are multiple of 8
    for (int hsize = 8; hsize <= 64; hsize += 8)
        for (int vsize = 8; vsize <= 64; vsize += 8) {
            memset(dst_data_tst_, 0, sizeof(dst_data_tst_));
            memset(dst_data_ref_, 0, sizeof(dst_data_ref_));
            uint8_t *src_ =
                src_data_ + CDEF_VBORDER * CDEF_BSTRIDE + CDEF_HBORDER;
            uint16_t *dst_tst_ =
                dst_data_tst_ + CDEF_VBORDER * CDEF_BSTRIDE + CDEF_HBORDER;
            uint16_t *dst_ref_ =
                dst_data_ref_ + CDEF_VBORDER * CDEF_BSTRIDE + CDEF_HBORDER;

            eb_copy_rect8_8bit_to_16bit_c(
                dst_ref_, CDEF_BSTRIDE, src_, CDEF_BSTRIDE, vsize, hsize);
            eb_copy_rect8_8bit_to_16bit_avx2(
                dst_tst_, CDEF_BSTRIDE, src_, CDEF_BSTRIDE, vsize, hsize);

            for (int i = 0; i < vsize; ++i) {
                for (int j = 0; j < hsize; ++j)
                    ASSERT_EQ(dst_ref_[i * CDEF_BSTRIDE + j],
                              dst_ref_[i * CDEF_BSTRIDE + j])
                        << "copy_rect8_8bit_to_16bit failed with pos(" << i
                        << " " << j << ")";
            }
        }
}

/**
 * @brief Unit test for compute_cdef_dist_avx2
 *
 * Test strategy:
 * Feed cdef list, src buffer, dst buffer generated randomly to targeted
 * and reference functions, and compare mse returned.
 *
 * Expect result:
 * The best mse from targeted function should be identical
 * with the mse from reference function.
 *
 * Test coverage:
 * Test cases:
 * bitdepth: 8, 10, 12
 * BlockSize: {BLOCK_4X4, BLOCK_4X8, BLOCK_8X4, BLOCK_8X8}
 * Pli: 0, 1, 2
 *
 */
TEST(CdefToolTest, ComputeCdefDistMatchTest) {
    const int stride = 1 << MAX_SB_SIZE_LOG2;
    const int buf_size = 1 << (MAX_SB_SIZE_LOG2 * 2);
    DECLARE_ALIGNED(32, uint16_t, src_data_[buf_size]);
    DECLARE_ALIGNED(32, uint16_t, dst_data_[buf_size]);

    // compute cdef list
    for (int bd = 8; bd <= 12; ++bd) {
        // prepare src data
        SVTRandom rnd_(bd, false);
        for (int i = 0; i < buf_size; ++i) {
            src_data_[i] = rnd_.random();
            dst_data_[i] = rnd_.random();
        }

        const int coeff_shift = bd - 8;
        SVTRandom skip_rnd_(0, 1);
        for (int k = 0; k < 100; ++k) {
            cdef_list dlist[MI_SIZE_128X128 * MI_SIZE_128X128];
            int cdef_count = 0;

            // generate the cdef list randomly
            for (int r = 0; r < MI_SIZE_128X128; r += 2) {
                for (int c = 0; c < MI_SIZE_128X128; c += 2) {
                    // append non-skip block into dlist
                    if (!skip_rnd_.random()) {
                        dlist[cdef_count].by = (uint8_t)(r >> 1);
                        dlist[cdef_count].bx = (uint8_t)(c >> 1);
                        ++cdef_count;
                    }
                }
            }

            const BlockSize test_bs[] = {
                BLOCK_4X4, BLOCK_4X8, BLOCK_8X4, BLOCK_8X8};
            for (int i = 0; i < 4; ++i) {
                for (int plane = 0; plane < 3; ++plane) {
                    const uint64_t c_mse = compute_cdef_dist_c(dst_data_,
                                                               stride,
                                                               src_data_,
                                                               dlist,
                                                               cdef_count,
                                                               test_bs[i],
                                                               coeff_shift,
                                                               plane);
                    const uint64_t avx_mse = compute_cdef_dist_avx2(dst_data_,
                                                                    stride,
                                                                    src_data_,
                                                                    dlist,
                                                                    cdef_count,
                                                                    test_bs[i],
                                                                    coeff_shift,
                                                                    plane);
                    ASSERT_EQ(c_mse, avx_mse)
                        << "compute_cdef_dist_avx2 failed "
                        << "bitdepth: " << bd << " plane: " << plane
                        << " BlockSize " << test_bs[i] << " loop: " << k;
                }
            }
        }
    }
}

TEST(CdefToolTest, ComputeCdefDist8bitMatchTest) {
    const int stride = 1 << MAX_SB_SIZE_LOG2;
    const int buf_size = 1 << (MAX_SB_SIZE_LOG2 * 2);
    DECLARE_ALIGNED(32, uint8_t, src_data_[buf_size]);
    DECLARE_ALIGNED(32, uint8_t, dst_data_[buf_size]);

    // compute cdef list
    for (int bd = 8; bd <= 12; ++bd) {
        // prepare src data
        SVTRandom rnd_(bd, false);
        for (int i = 0; i < buf_size; ++i) {
            src_data_[i] = rnd_.random()%255;
            dst_data_[i] = rnd_.random()%255;
        }

        const int coeff_shift = bd - 8;
        SVTRandom skip_rnd_(0, 1);
        for (int k = 0; k < 100; ++k) {
            cdef_list dlist[MI_SIZE_128X128 * MI_SIZE_128X128];
            int cdef_count = 0;

            // generate the cdef list randomly
            for (int r = 0; r < MI_SIZE_128X128; r += 2) {
                for (int c = 0; c < MI_SIZE_128X128; c += 2) {
                    // append non-skip block into dlist
                    if (!skip_rnd_.random()) {
                        dlist[cdef_count].by = (uint8_t)(r >> 1);
                        dlist[cdef_count].bx = (uint8_t)(c >> 1);
                        ++cdef_count;
                    }
                }
            }

            const BlockSize test_bs[] = {
                BLOCK_4X4, BLOCK_4X8, BLOCK_8X4, BLOCK_8X8};
            for (int i = 0; i < 4; ++i) {
                for (int plane = 0; plane < 3; ++plane) {
                    const uint64_t c_mse = compute_cdef_dist_8bit_c(dst_data_,
                                                               stride,
                                                               src_data_,
                                                               dlist,
                                                               cdef_count,
                                                               test_bs[i],
                                                               coeff_shift,
                                                               plane);
                    const uint64_t avx_mse = compute_cdef_dist_8bit_avx2(dst_data_,
                                                                    stride,
                                                                    src_data_,
                                                                    dlist,
                                                                    cdef_count,
                                                                    test_bs[i],
                                                                    coeff_shift,
                                                                    plane);
                    ASSERT_EQ(c_mse, avx_mse)
                        << "compute_cdef_dist_8bit_avx2 failed "
                        << "bitdepth: " << bd << " plane: " << plane
                        << " BlockSize " << test_bs[i] << " loop: " << k;
                }
            }
        }
    }
}

/**
 * @brief Unit test for search_one_dual_avx2
 *
 * Test strategy:
 * Prepare the mse array filled randomly and check the best mse, best
 * strength for luma and chroma returned from refrence
 * function and targeted function.
 *
 * Expect result:
 * The best mse and strengths from targeted function should be identical
 * with the values from reference function.
 *
 * Test coverage:
 * Test cases:
 * nb_strength: [0 2)
 * end_gi: TOTAL_STRENGTHS
 *
 */
TEST(CdefToolTest, SearchOneDualMatchTest) {
    // setup enviroment
    const int sb_count = 100;
    const int fast = 0;  // unused
    const int start_gi = 0;
    const int end_gi = TOTAL_STRENGTHS;
    int lvl_luma_ref[CDEF_MAX_STRENGTHS], lvl_chroma_ref[CDEF_MAX_STRENGTHS];
    int lvl_luma_tst[CDEF_MAX_STRENGTHS], lvl_chroma_tst[CDEF_MAX_STRENGTHS];
    uint64_t(*mse[2])[TOTAL_STRENGTHS];
    mse[0] = (uint64_t(*)[64])eb_aom_memalign(32, sizeof(**mse) * sb_count);
    mse[1] = (uint64_t(*)[64])eb_aom_memalign(32, sizeof(**mse) * sb_count);

    SVTRandom rnd_(10, false);
    for (int k = 0; k < 100; ++k) {
        // generate mse randomly
        for (int i = 0; i < 2; ++i)
            for (int n = 0; n < sb_count; ++n)
                for (int j = 0; j < 64; ++j)
                    mse[i][n][j] = rnd_.random();

        // try different nb_strengths
        for (int i = 0; i < 2; ++i) {
            memset(lvl_luma_ref, 0, sizeof(lvl_luma_ref));
            memset(lvl_chroma_ref, 0, sizeof(lvl_chroma_ref));
            memset(lvl_luma_tst, 0, sizeof(lvl_luma_tst));
            memset(lvl_chroma_tst, 0, sizeof(lvl_chroma_tst));

            int nb_strengths = 1 << i;
            for (int j = 0; j < nb_strengths; ++j) {
                uint64_t best_mse_ref = search_one_dual_c(lvl_luma_ref,
                                                          lvl_chroma_ref,
                                                          j,
                                                          mse,
                                                          sb_count,
                                                          fast,
                                                          start_gi,
                                                          end_gi);
                uint64_t best_mse_tst = search_one_dual_avx2(lvl_luma_tst,
                                                             lvl_chroma_tst,
                                                             j,
                                                             mse,
                                                             sb_count,
                                                             fast,
                                                             start_gi,
                                                             end_gi);

                ASSERT_EQ(best_mse_tst, best_mse_ref)
                    << "search_one_dual_avx2 return different best mse "
                    << "loop: " << k << " nb_strength: " << nb_strengths;
                for (int h = 0; h < CDEF_MAX_STRENGTHS; ++h) {
                    ASSERT_EQ(lvl_luma_ref[h], lvl_luma_tst[h])
                        << "best strength for luma does not match "
                        << "loop: " << k << " nb_strength: " << nb_strengths
                        << " pos " << h;
                    ASSERT_EQ(lvl_chroma_ref[h], lvl_chroma_tst[h])
                        << "best strength for chroma does not match "
                        << "loop: " << k << " nb_strength: " << nb_strengths
                        << " pos " << h;
                }
            }
        }
    }

    eb_aom_free(mse[0]);
    eb_aom_free(mse[1]);
}
