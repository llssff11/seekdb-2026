/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ik/ob_ik_cjk_processor.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
int ObIKCJKProcessor::do_process(TokenizeContext &ctx,
                                 const char *ch,
                                 const uint8_t char_len,
                                 const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;

  if (ObFTCharUtil::CharType::USELESS != type) {
    // handle the pending hits first and then check from this char.  The entries
    // that are still alive are compacted in place, so the ones that turned out
    // to be unmatched are dropped without touching the allocator.
    int64_t kept = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < hits_cnt_; ++i) {
      ObDATrieHit &hit = hits_[i];
      if (OB_FAIL(dict_main_.match_with_hit({char_len, ch}, hit, hit))) {
      } else if (hit.is_match()) {
        if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                  hit.start_pos_,
                                  hit.end_pos_ - hit.start_pos_,
                                  hit.char_cnt_,
                                  ObIKTokenType::IK_CHINESE_TOKEN))) {
        } else if (kept != i) {
          hits_[kept] = hit;
        }
        ++kept;
      } else if (hit.is_prefix()) {
        if (kept != i) {
          hits_[kept] = hit;
        }
        ++kept;
      } else if (hit.is_unmatch()) {
        // drop this prefix
      } else {
        ret = OB_UNEXPECT_INTERNAL_ERROR;
      }
    }
    hits_cnt_ = kept;

    if (OB_SUCC(ret)) {
      // Start from this char
      // find range of dict
      ObDATrieHit hit(&dict_main_, ctx.get_cursor());
      if (OB_FAIL(dict_main_.match({char_len, ch}, hit))) {
      } else if (!hit.is_match() && !hit.is_prefix()) {
        // ignore mismatch
      } else if (OB_UNLIKELY(hits_cnt_ >= MAX_PENDING_HITS)) {
        // Unreachable while the dictionary words are shorter than MAX_PENDING_HITS.
        if (hit.is_match()) {
          ctx.add_token(ctx.fulltext(),
                        ctx.get_cursor(),
                        char_len,
                        1,
                        ObIKTokenType::IK_CHINESE_TOKEN);
        }
      } else {
        hits_[hits_cnt_++] = hit;
        if (hit.is_match()) {
          // output token
          ctx.add_token(ctx.fulltext(),
                        ctx.get_cursor(),
                        char_len,
                        1,
                        ObIKTokenType::IK_CHINESE_TOKEN);
        }
      }
    }
  } else {
    // stop previous match
    hits_cnt_ = 0;
  }

  if (OB_FAIL(ret) || ctx.is_last()) {
    hits_cnt_ = 0;
  }

  return ret;
}

} //  namespace storage
} //  namespace oceanbase
