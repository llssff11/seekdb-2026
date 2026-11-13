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

#ifndef _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_
#define _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_

#include "lib/allocator/ob_allocator.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_processor.h"

namespace oceanbase
{
namespace storage
{
class ObFTDictHub;
class ObIKCJKProcessor : public ObIIKProcessor
{
public:
  // The hit list only ever holds the prefixes that are still alive, which is
  // bounded by the length of the longest word of the dictionary.  Keeping it in
  // a fixed array avoids one allocator round trip per character for the append
  // and another one for the removal of the dead prefixes.
  static const int64_t MAX_PENDING_HITS = 64;

  ObIKCJKProcessor(const ObIFTDict &dict_main, ObIAllocator &alloc)
      : hits_cnt_(0), dict_main_(dict_main), cjk_start_(-1), cjk_end_(-1)
  {
    (void)alloc;
  }
  ~ObIKCJKProcessor() override {}

public:
  int do_process(TokenizeContext &ctx,
                 const char *ch,
                 const uint8_t char_len,
                 const ObFTCharUtil::CharType type) override;

private:
  ObDATrieHit hits_[MAX_PENDING_HITS];
  int64_t hits_cnt_;
  const ObIFTDict &dict_main_;
  int64_t cjk_start_;
  int64_t cjk_end_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObIKCJKProcessor);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_
