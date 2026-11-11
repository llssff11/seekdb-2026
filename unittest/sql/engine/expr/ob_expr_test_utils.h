/**
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

#pragma once

/**
 * The common utilities of the expression unit tests.
 *
 * Only the lightweight helpers are collected here, so that the test cases of
 * the ai functions do not depend on the whole expression test kit.
 */

#include "lib/allocator/ob_malloc.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_array.h"
#include "lib/string/ob_string.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_tree.h"
#include "share/json/ob_json_helper.h"
