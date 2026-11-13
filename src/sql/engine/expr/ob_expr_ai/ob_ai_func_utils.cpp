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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_ai_func_utils.h"
#include "ob_ai_func_client.h"
#include "query/engine/expr/ob_ai_model_resolver.h"

namespace oceanbase 
{
namespace common 
{

int ObOpenAIUtils::get_header(common::ObIAllocator &allocator,
                              ObString &api_key,
                              common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (api_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //["Authorization: Bearer %.*s", "Content-Type: application/json"]
    int auth_header_len = 1024;
    char *auth_header_str = (char *)allocator.alloc(auth_header_len);
    ObString content_type_str("Content-Type: application/json");
    ObString content_type_c_str;
    if (OB_FAIL(ob_write_string(allocator, content_type_str, content_type_c_str, true))) {
    } else if (OB_ISNULL(auth_header_str)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("Failed to allocate memory for auth header string and content type string", K(ret));
    } else {
      int auth_header_pos = snprintf(auth_header_str, auth_header_len,
                         "Authorization: Bearer %.*s", api_key.length(), api_key.ptr());
      if (auth_header_pos < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(headers.push_back(ObString(auth_header_str)))) {
      } else if (OB_FAIL(headers.push_back(content_type_c_str))) {
      }
    }
  }
  return ret;
}
int ObOpenAIUtils::ObOpenAIComplete::get_header(common::ObIAllocator &allocator,
                                                ObString &api_key,
                                                common::ObArray<ObString> &headers) 
{
  return ObOpenAIUtils::get_header(allocator, api_key, headers);
}

int ObOpenAIUtils::ObOpenAIComplete::get_body(common::ObIAllocator &allocator,
                                              common::ObString &model,
                                              common::ObString &prompt,
                                              common::ObString &content,
                                              common::ObJsonObject *config,
                                              common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "messages": [{"role": "system", "content": "*"}, {"role": "user", "content": "*"}]}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *messages_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(construct_messages_array(allocator, prompt, content, messages_array))) {
    } else if (OB_FAIL(body_obj->add("messages", messages_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object(allocator, config, body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::construct_messages_array(ObIAllocator &allocator, ObString &prompt, ObString &content, ObJsonArray *&messages)
{
  int ret = OB_SUCCESS;
  if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //messages: [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": "What is the capital of France?"}]
    ObJsonArray *messages_array = nullptr;
    ObJsonObject *sys_message_obj = nullptr;
    ObJsonObject *user_message_obj = nullptr;
    ObString system_str("system");
    ObString user_str("user");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, messages_array))) {
    } else if(!prompt.empty()) {
      if (OB_FAIL(construct_message_obj(allocator, system_str, prompt, sys_message_obj))) {
      } else if (OB_FAIL(messages_array->append(sys_message_obj))) {
      }
    } 
    if (OB_SUCC(ret)) {
      if (OB_FAIL(construct_message_obj(allocator, user_str, content, user_message_obj))) {
      } else if (OB_FAIL(messages_array->append(user_message_obj))) {
      } else {
        messages = messages_array;
      }
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::construct_message_obj(ObIAllocator &allocator, ObString &role, ObString &content, ObJsonObject *&message)
{
  int ret = OB_SUCCESS;
  if (role.empty() || content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *message_obj = nullptr;
    ObJsonString *role_json_str = nullptr;
    ObJsonString *content_json_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, message_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, role, role_json_str))) {
    } else if (OB_FAIL(message_obj->add("role", role_json_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, content, content_json_str))) {
    } else if (OB_FAIL(message_obj->add("content", content_json_str))) {
    } else {
      message = message_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"response_format":{"type":"json_object"}}
    ObString json_str("json_object");
    ObJsonString *json_str_obj = nullptr;
    ObJsonObject *json_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, json_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, json_str, json_str_obj))) {
    } else if (OB_FAIL(json_obj->add("type", json_str_obj))) {
    } else if (OB_FAIL(config->add("response_format", json_obj))) {
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::parse_output(common::ObIAllocator &allocator, 
                                                  common::ObJsonObject *http_response,
                                                  common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObIJsonBase *j_tree = http_response;
    common::ObString path_text("$.choices[0].message.content");
    ObJsonPath j_path(path_text, &allocator);
    ObJsonSeekResult hit;
    if (OB_FAIL(j_path.parse_path())) {
    } else if (OB_FAIL(j_tree->seek(j_path, j_path.path_node_cnt(), false, false, hit))) {
    } else if (hit.size() == 0) {
      ret = OB_INVALID_DATA;
    } else {
      result = hit[0];
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::get_header(common::ObIAllocator &allocator,
                                             ObString &api_key,
                                             common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::get_body(common::ObIAllocator &allocator,
                                           common::ObString &model,
                                           common::ObArray<ObString> &contents,
                                           common::ObJsonObject *config,
                                           common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *input_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, input_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object(allocator, config, body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::get_body(common::ObIAllocator &allocator,
                                           common::ObString &model,
                                           common::ObArray<ObString> &contents,
                                           common::ObJsonObject *config,
                                           common::ObString &input_type,
                                           common::ObJsonObject *&body)
{
  // the openai compatible protocol does not distinguish the type of the input
  // content, the input type is only used by the providers which support the
  // multi modal input
  UNUSED(input_type);
  return get_body(allocator, model, contents, config, body);
}

// get the embedding node and the index node of one element of the embedding response.
// the index is optional, the model service may not return it, and the position of the
// element in the response is used as its index in that case
static int get_embedding_element(ObJsonArray *data_array,
                                 const int64_t pos,
                                 ObJsonNode *&embedding,
                                 ObJsonNode *&index_node)
{
  int ret = OB_SUCCESS;
  ObJsonNode *element = nullptr;
  embedding = nullptr;
  index_node = nullptr;
  if (OB_ISNULL(data_array)) {
    ret = OB_INVALID_DATA;
  } else if (OB_ISNULL(element = data_array->get_value(static_cast<uint64_t>(pos)))
             || element->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else {
    ObJsonObject *element_obj = static_cast<ObJsonObject *>(element);
    if (OB_ISNULL(embedding = element_obj->get_value("embedding"))
        || embedding->json_type() == ObJsonNodeType::J_NULL) {
      ret = OB_INVALID_DATA;
    } else {
      index_node = element_obj->get_value("index");
    }
  }
  return ret;
}

/**
 * @brief Append the embeddings of the response to the result array in the order of the
 *        inputs. The model service is allowed to return the embeddings in a different
 *        order than the inputs, every element carries the index of its input then. The
 *        order of the response is kept when the index of some element is missing or
 *        invalid, so that the response of the old model services is still accepted.
 */
static int append_embeddings_in_input_order(ObIAllocator &allocator,
                                            ObJsonArray *data_array,
                                            ObJsonArray *&result_array)
{
  int ret = OB_SUCCESS;
  ObJsonArray *ordered_array = nullptr;
  ObJsonNode **index_slots = nullptr;
  const int64_t count = OB_ISNULL(data_array) ? 0 : static_cast<int64_t>(data_array->element_count());
  bool index_is_usable = (count > 0);
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, ordered_array))) {
  } else if (count <= 0) {
    // the empty response is returned as the empty array
  } else if (OB_ISNULL(index_slots = static_cast<ObJsonNode **>(allocator.alloc(sizeof(ObJsonNode *) * count)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMSET(index_slots, 0, sizeof(ObJsonNode *) * count);
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *embedding = nullptr;
      ObJsonNode *index_node = nullptr;
      if (OB_FAIL(get_embedding_element(data_array, i, embedding, index_node))) {
      } else {
        int64_t index = i;
        if (OB_NOT_NULL(index_node)) {
          if (ObJsonNodeType::J_INT == index_node->json_type()) {
            index = index_node->get_int();
          } else if (ObJsonNodeType::J_UINT == index_node->json_type()) {
            index = static_cast<int64_t>(index_node->get_uint());
          } else {
            index_is_usable = false;
          }
        }
        if (index < 0 || index >= count || OB_NOT_NULL(index_slots[index])) {
          // the index of the element is out of the range or duplicated
          index_is_usable = false;
        } else {
          index_slots[index] = embedding;
        }
      }
    }
    if (OB_SUCC(ret) && index_is_usable) {
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        if (OB_FAIL(ordered_array->append(index_slots[i]))) {
        }
      }
    } else if (OB_SUCC(ret)) {
      // the order of the response is kept
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        ObJsonNode *embedding = nullptr;
        ObJsonNode *index_node = nullptr;
        if (OB_FAIL(get_embedding_element(data_array, i, embedding, index_node))) {
        } else if (OB_FAIL(ordered_array->append(embedding))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    result_array = ordered_array;
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::parse_output(common::ObIAllocator &allocator, 
                                               common::ObJsonObject *http_response,
                                               common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *result_array = nullptr;
    ObJsonNode *data_node = nullptr;
    if (OB_ISNULL(data_node = http_response->get_value("data"))) {
      ret = OB_INVALID_DATA;
    } else if (data_node->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(append_embeddings_in_input_order(allocator,
                                                       static_cast<ObJsonArray *>(data_node),
                                                       result_array))) {
    } else {
      result = result_array;
    }
  }
  return ret;
}

// the ollama service accepts the requests without the authorization header,
// but the access key can still be used, and the http client requires a non
// empty header list
static int ob_ollama_add_auth_header(common::ObIAllocator &allocator,
                                     common::ObString &api_key,
                                     common::ObArray<ObString> &headers)
{
  int ret = OB_SUCCESS;
  int64_t buf_len = api_key.length() + 32;
  char *buf = static_cast<char *>(allocator.alloc(buf_len));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t len = snprintf(buf, buf_len, "Authorization: Bearer %.*s",
                            static_cast<int>(api_key.length()), api_key.ptr());
    if (len <= 0 || len >= buf_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObString auth_header;
      auth_header.assign_ptr(buf, static_cast<ObString::obstr_size_t>(len));
      if (OB_FAIL(headers.push_back(auth_header))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  return ret;
}

int ObOllamaUtils::get_header(common::ObIAllocator &allocator,
                              common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  ObString content_type("Content-Type: application/json");
  if (OB_FAIL(headers.push_back(content_type))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::get_header(common::ObIAllocator &allocator,
                                                common::ObString &api_key,
                                                common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOllamaUtils::get_header(allocator, headers))) {
  } else if (!api_key.empty()) {
    ret = ob_ollama_add_auth_header(allocator, api_key, headers);
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::get_body(common::ObIAllocator &allocator,
                                              common::ObString &model,
                                              common::ObString &prompt,
                                              common::ObString &content,
                                              common::ObJsonObject *config,
                                              common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || content.empty()) { 
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "llama3.1", "prompt": "What is the capital of France?"}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonString *content_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, content, content_str))) {
    } else if (OB_FAIL(body_obj->add("prompt", content_str))) {
    } else {
      // copy the config members, the stream option is always disabled because
      // the http client can only parse one json object from the response
      if (OB_NOT_NULL(config)) {
        ObString key;
        int64_t count = config->element_count();
        for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
          ObJsonNode *j_node = config->get_value(i);
          if (OB_ISNULL(j_node)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(config->get_key(i, key))) {
          } else if (key.case_compare("model") == 0 || key.case_compare("prompt") == 0
                     || key.case_compare("stream") == 0) {
            // skip the options which are managed by the ai function itself
          } else if (OB_FAIL(body_obj->add(key, j_node))) {
          }
        }
      }
      if (OB_SUCC(ret)) {
        ObJsonBoolean *stream_node = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, false, stream_node))) {
        } else if (OB_FAIL(body_obj->add("stream", stream_node))) {
        } else {
          body = body_obj;
        }
      }
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"format": "json"}
    ObString json_str("json");
    ObJsonString *json_str_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, json_str, json_str_obj))) {
    } else if (OB_FAIL(config->add("format", json_str_obj))) {
    } 
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::parse_output(common::ObIAllocator &allocator,
                                                  common::ObJsonObject *http_response,
                                                  common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObIJsonBase *j_tree = http_response;
    common::ObString path_text("$.response");
    ObJsonPath j_path(path_text, &allocator);
    ObJsonSeekResult hit;
    if (OB_FAIL(j_path.parse_path())) {
    } else if (OB_FAIL(j_tree->seek(j_path, j_path.path_node_cnt(), false, false, hit))) {
    } else if (hit.size() == 0) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      result = hit[0];
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaEmbed::get_header(common::ObIAllocator &allocator,
                                             ObString &api_key,
                                             common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOllamaUtils::get_header(allocator, headers))) {
  } else if (!api_key.empty()) {
    ret = ob_ollama_add_auth_header(allocator, api_key, headers);
  }
  return ret;
}

int ObOllamaUtils::ObOllamaEmbed::get_body(common::ObIAllocator &allocator,
                                           common::ObString &model,
                                           common::ObArray<ObString> &contents,
                                           common::ObJsonObject *config,
                                           common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *input_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, input_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_array))) {
    } else {
      body = body_obj;
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object(allocator, config, body))) {
      }
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaEmbed::parse_output(common::ObIAllocator &allocator,
                                               common::ObJsonObject *http_response,
                                               common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObIJsonBase *j_tree = http_response;
    common::ObString path_text("$.embeddings");
    ObJsonPath j_path(path_text, &allocator);
    ObJsonSeekResult hit;
    if (OB_FAIL(j_path.parse_path())) {
    } else if (OB_FAIL(j_tree->seek(j_path, j_path.path_node_cnt(), false, false, hit))) {
    } else if (hit.size() == 0) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      result = hit[0];
    }
  }
  return ret;
}

int ObDashscopeUtils::get_header(common::ObIAllocator &allocator,
                                 common::ObString &api_key,
                                 common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::get_header(common::ObIAllocator &allocator, 
                                                      common::ObString &api_key,
                                                      common::ObArray<ObString> &headers) 
{
  return ObOpenAIUtils::get_header(allocator, api_key, headers);
}

int ObDashscopeUtils::ObDashscopeComplete::get_body(common::ObIAllocator &allocator,
                                                    common::ObString &model,
                                                    common::ObString &prompt,
                                                    common::ObString &content,
                                                    common::ObJsonObject *config,
                                                    common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(content)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "input": {"messages": [{"role": "system", "content": "*"}, {"role": "user", "content": "*"}]}, "parameters": {}}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;

    if (OB_ISNULL(config)) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
      } else if (OB_FAIL(body_obj->add("model", model_str))) {
      } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeComplete::construct_input_obj(allocator, prompt, content, input_obj))) {
      } else if (OB_FAIL(body_obj->add("input", input_obj))) {
      } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeComplete::set_config_result_format(allocator, config))) {
      } else if (OB_FAIL(body_obj->add("parameters", config))) {
      } else {
        body = body_obj;
      }
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::set_config_result_format(ObIAllocator &allocator, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_NOT_NULL(config->get_value("result_format"))) {
    if (OB_FAIL(config->remove("result_format"))) {
    }
  } 
  if (OB_SUCC(ret)) {
    // {"result_format": "message"}
    ObJsonString *result_format_str = nullptr;
    ObString result_format_str_val("message");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, result_format_str_val, result_format_str))) {
    } else if (OB_FAIL(config->add("result_format", result_format_str))) {
    }
  } 
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::construct_input_obj(ObIAllocator &allocator, ObString &prompt, ObString &content, ObJsonObject *&input_obj)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(content)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *obj = nullptr;
    ObJsonArray *messages_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
    } else if (OB_FAIL(ObOpenAIUtils::ObOpenAIComplete::construct_messages_array(allocator, prompt, content, messages_array))) {
    } else if (OB_FAIL(obj->add("messages", messages_array))) {
    } else {
      input_obj = obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::set_config_json_format(ObIAllocator &allocator, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //{"type": "json_object"}
    ObJsonString *type_str = nullptr;
    ObString type_str_val("json_object");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, type_str_val, type_str))) {
    } else if (OB_FAIL(config->add("type", type_str))) {
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::parse_output(ObIAllocator &allocator, ObJsonObject *http_response, ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *output_obj = nullptr;
    ObJsonArray *choices_array = nullptr;
    ObJsonObject *choice_obj = nullptr;
    ObJsonObject *message_obj = nullptr;
    ObJsonString *content_str = nullptr;
    ObString response_str;
    if (OB_ISNULL(output_obj = static_cast<ObJsonObject *>(http_response->get_value("output")))) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(choices_array = static_cast<ObJsonArray *>(output_obj->get_value("choices")))) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(choice_obj = static_cast<ObJsonObject *>(choices_array->get_value(0)))) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(message_obj = static_cast<ObJsonObject *>(choice_obj->get_value("message")))) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(content_str = static_cast<ObJsonString *>(message_obj->get_value("content")))) {
      ret = OB_INVALID_DATA;
    } else {
      result = content_str;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeEmbed::get_header(common::ObIAllocator &allocator,
                                                   common::ObString &api_key,
                                                   common::ObArray<ObString> &headers) 
{
  return ObDashscopeUtils::get_header(allocator, api_key, headers);
}

int ObDashscopeUtils::ObDashscopeEmbed::get_body(common::ObIAllocator &allocator,
                                                 common::ObString &model,
                                                 common::ObArray<ObString> &contents,
                                                 common::ObJsonObject *config,
                                                 common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "input": {"texts": ["*"]}, "parameters": {}}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;
    ObJsonArray *texts_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, input_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, texts_array))) {
    } else if (OB_FAIL(input_obj->add("texts", texts_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_obj))) {
    } else if (config != nullptr && config->element_count() > 0) {
      ObJsonNode *dimensions_node = config->get_value("dimensions");
      if (OB_ISNULL(dimensions_node)) {
        // do nothing
      } else if (OB_FAIL(config->rename_key("dimensions", "dimension"))) {
      } 
      if (OB_SUCC(ret)) {
        if (OB_FAIL(body_obj->add("parameters", config))) {
        }
      }
    } 
    if (OB_SUCC(ret)) {
      body = body_obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeEmbed::parse_output(common::ObIAllocator &allocator,
                                                    common::ObJsonObject *http_response,
                                                    common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"output": {"embeddings": [{"embedding": ["*"], "index": 0}]}}
    ObJsonObject *output_obj = nullptr;
    ObJsonArray *embeddings_array = nullptr;
    ObJsonArray *result_array = nullptr;
    if (OB_ISNULL(output_obj = static_cast<ObJsonObject *>(http_response->get_value("output")))) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(embeddings_array = static_cast<ObJsonArray *>(output_obj->get_value("embeddings")))) {
      ret = OB_INVALID_DATA;
    } else if (embeddings_array->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(append_embeddings_in_input_order(allocator, embeddings_array, result_array))) {
    } else {
      result = result_array;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_header(common::ObIAllocator &allocator,
                                                    common::ObString &api_key,
                                                    common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_body(common::ObIAllocator &allocator,
                                                  common::ObString &model,
                                                  common::ObString &query,
                                                  common::ObJsonArray *document_array,
                                                  common::ObJsonObject *config,
                                                  common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeRerank::get_input_obj(allocator, query, document_array, input_obj))) {
    } else if (OB_FAIL(body_obj->add("input", input_obj))) {
    } else if (config != nullptr && config->element_count() > 0) {
      if (OB_FAIL(body_obj->add("parameters", config))) {
      }
    } 
    if (OB_SUCC(ret)) {
      body = body_obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_input_obj(common::ObIAllocator &allocator,
                                                      common::ObString &query,
                                                      common::ObJsonArray *document_array,
                                                      common::ObJsonObject *&input_obj) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *obj = nullptr;
    ObJsonString *query_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, query, query_str))) {
    } else if (OB_FAIL(obj->add("query", query_str))) {
    } else if (OB_FAIL(obj->add("documents", document_array))) {
    } else {
      input_obj = obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::parse_output(common::ObIAllocator &allocator,
                                                      common::ObJsonObject *http_response,
                                                      common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObJsonObject *output_obj = nullptr;
  ObJsonArray *results_array = nullptr;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(output_obj = static_cast<ObJsonObject *>(http_response->get_value("output")))) {
    ret = OB_INVALID_DATA;
  } else if (OB_ISNULL(results_array = static_cast<ObJsonArray *>(output_obj->get_value("results")))) {
    ret = OB_INVALID_DATA;
  } else {
    result = results_array;
  }
  return ret;
}

int ObSiliconflowUtils::get_header(common::ObIAllocator &allocator,
                                 common::ObString &api_key,
                                 common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::get_header(common::ObIAllocator &allocator,
                                                    common::ObString &api_key,
                                                    common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::get_body(common::ObIAllocator &allocator,
                                                  common::ObString &model,
                                                  common::ObString &query,
                                                  common::ObJsonArray *document_array,
                                                  common::ObJsonObject *config,
                                                  common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonString *query_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, query, query_str))) {
    } else if (OB_FAIL(body_obj->add("query", query_str))) {
    } else if (OB_FAIL(body_obj->add("documents", document_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object(allocator, config, body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::parse_output(common::ObIAllocator &allocator,
                                                      common::ObJsonObject *http_response,
                                                      common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObJsonArray *results_array = nullptr;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(results_array = static_cast<ObJsonArray *>(http_response->get_value("results")))) {
    ret = OB_INVALID_DATA;
  } else {
    result = results_array;
  }
  return ret;
}


int ObAIFuncUtils::get_header(ObIAllocator &allocator, 
                              const ObAIFuncExprInfo &info,
                              const share::ObAiModelEndpointInfo &endpoint_info,
                              ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  ObString unencrypted_access_key;
  if (OB_FAIL(endpoint_info.get_unencrypted_access_key(allocator, unencrypted_access_key))) {
  } else if (ObAIFuncUtils::is_completion_type(&info)) {
    ObAIFuncIComplete *complete_provider = nullptr;
    if (OB_FAIL(get_complete_provider(allocator, endpoint_info.get_provider(), complete_provider))) {
    } else if (OB_FAIL(complete_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  } else if (ObAIFuncUtils::is_dense_embedding_type(&info)) {
    ObAIFuncIEmbed *embed_provider = nullptr;
    if (OB_FAIL(get_embed_provider(allocator, endpoint_info.get_provider(), embed_provider))) {
    } else if (OB_FAIL(embed_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  } else if (ObAIFuncUtils::is_rerank_type(&info)) {
    ObAIFuncIRerank *rerank_provider = nullptr;
    if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
    } else if (OB_FAIL(rerank_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  }
  return ret;
}

int ObAIFuncUtils::get_complete_body(ObIAllocator &allocator, 
                                    const ObAIFuncExprInfo &info, 
                                    const share::ObAiModelEndpointInfo &endpoint_info,
                                    ObString &prompt, 
                                    ObString &content, 
                                    ObJsonObject *config,
                                    ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, endpoint_info.get_provider(), complete_provider))) {
  } else if (OB_FAIL(complete_provider->get_body(allocator, request_model_name, prompt, content, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::set_json_format_config(ObIAllocator &allocator, const ObString &provider, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, provider, complete_provider))) {
  } else if (OB_FAIL(complete_provider->set_config_json_format(allocator, config))) {
  } 
  return ret;
}

int ObAIFuncUtils::get_embed_body(ObIAllocator &allocator, 
                                  const ObAIFuncExprInfo &info, 
                                  const share::ObAiModelEndpointInfo &endpoint_info,
                                  ObArray<ObString> &contents, 
                                  ObJsonObject *config,
                                  ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIEmbed *embed_provider = nullptr;
  if (OB_FAIL(get_embed_provider(allocator, endpoint_info.get_provider(), embed_provider))) {
  } else if (OB_FAIL(embed_provider->get_body(allocator, request_model_name, contents, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::get_rerank_body(ObIAllocator &allocator, 
                                   const ObAIFuncExprInfo &info,
                                   const share::ObAiModelEndpointInfo &endpoint_info,
                                   ObString &query,
                                   ObJsonArray *document_array,
                                   ObJsonObject *config,
                                   ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIRerank *rerank_provider = nullptr;
  if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(rerank_provider->get_body(allocator, request_model_name, query, document_array, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_complete_output(ObIAllocator &allocator, 
                                        const share::ObAiModelEndpointInfo &endpoint_info,
                                        ObJsonObject *http_response,
                                        ObIJsonBase *&result)                                                 
{
  int ret = OB_SUCCESS;
  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, endpoint_info.get_provider(), complete_provider))) {
  } else if (OB_FAIL(complete_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_embed_output(ObIAllocator &allocator, 
                                      const share::ObAiModelEndpointInfo &endpoint_info,
                                      ObJsonObject *http_response,
                                      ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObAIFuncIEmbed *embed_provider = nullptr;
  if (OB_FAIL(get_embed_provider(allocator, endpoint_info.get_provider(), embed_provider))) {
  } else if (OB_FAIL(embed_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_rerank_output(ObIAllocator &allocator, 
                                       const share::ObAiModelEndpointInfo &endpoint_info,
                                       ObJsonObject *http_response,
                                       ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObAIFuncIRerank *rerank_provider = nullptr;
  if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(rerank_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_object(ObIAllocator &allocator, ObJsonObject *&obj_node)
{
  int ret = OB_SUCCESS;
  ObJsonObject *j_obj = OB_NEWx(ObJsonObject, &allocator, &allocator);
  if (OB_ISNULL(j_obj)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    obj_node = j_obj;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_array(ObIAllocator &allocator, ObJsonArray *&array_node)
{
  int ret = OB_SUCCESS;
  ObJsonArray *j_array = OB_NEWx(ObJsonArray, &allocator, &allocator);
  if (OB_ISNULL(j_array)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    array_node = j_array; 
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_string(ObIAllocator &allocator, ObString &str, ObJsonString *&str_node)
{
  int ret = OB_SUCCESS;
  ObJsonString *j_str = OB_NEWx(ObJsonString, &allocator,str);
  if (OB_ISNULL(j_str)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    str_node = j_str;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_int(ObIAllocator &allocator, int64_t num, ObJsonInt *&int_node)
{
  int ret = OB_SUCCESS;
  ObJsonInt *j_int = OB_NEWx(ObJsonInt, &allocator, num);
  if (OB_ISNULL(j_int)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int_node = j_int;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_boolean(ObIAllocator &allocator, bool value, ObJsonBoolean *&boolean_node)
{
  int ret = OB_SUCCESS;
  ObJsonBoolean *j_bool = OB_NEWx(ObJsonBoolean, &allocator, value);
  if (OB_ISNULL(j_bool)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    boolean_node = j_bool;
  }
  return ret;
}

int ObAIFuncJsonUtils::print_json_to_str(ObIAllocator &allocator, ObIJsonBase *base_node, ObString &str)
{
  int ret = OB_SUCCESS;
  ObJsonBuffer j_buf(&allocator);
  if (OB_FAIL(base_node->print(j_buf, 0))) {
  } else {
    str = j_buf.string();
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_object_form_str(ObIAllocator &allocator, const ObString &str, ObJsonObject *&obj_node)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *j_base = NULL;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, str, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base))) {
  } else if (OB_ISNULL(j_base) || j_base->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_ERR_INVALID_JSON_TEXT;
    LOG_USER_ERROR(OB_ERR_INVALID_JSON_TEXT, "the config of ai function must be a json object");
  } else {
    obj_node = static_cast<ObJsonObject *>(j_base);
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_array_form_str(ObIAllocator &allocator, const ObString &str, ObJsonArray *&array_node)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *j_base = NULL;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, str, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base))) {
  } else if (OB_ISNULL(j_base) || j_base->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_INVALID_JSON_TEXT;
    LOG_USER_ERROR(OB_ERR_INVALID_JSON_TEXT, "the config of ai function must be a json array");
  } else {
    array_node = static_cast<ObJsonArray *>(j_base);
  }
  return ret;
}

int ObAIFuncJsonUtils::compact_json_object(ObIAllocator &allocator, ObJsonObject *obj_node, ObJsonObject *compact_obj)
{
  int ret = OB_SUCCESS;
  // add all members of obj_node to compact_obj
  if (OB_ISNULL(compact_obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(obj_node)) {
    // do nothing
  } else {
    ObString key;
    int64_t count = obj_node->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *j_node = obj_node->get_value(i);
      if (OB_ISNULL(j_node)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(obj_node->get_key(i, key))) {
      } else if (OB_FAIL(compact_obj->add(key, j_node))) {
      }
    }
  }
  return ret;
}

int ObAIFuncPromptUtils::replace_meta_prompt(ObIAllocator &allocator, ObString &meta_prompt, ObString &key, ObString &content, ObString &result)
{
  int ret = OB_SUCCESS;
  if (meta_prompt.empty() || key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *key_pos = nullptr;
    if (OB_ISNULL(key_pos = STRSTR(meta_prompt.ptr(), key.ptr()))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int64_t before_key_len = key_pos - meta_prompt.ptr();
      int64_t after_key_len = meta_prompt.length() - before_key_len - key.length();
      int64_t new_len = before_key_len + content.length() + after_key_len;
      char *new_str = static_cast<char *>(allocator.alloc(new_len));
      if (OB_ISNULL(new_str)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(new_str, meta_prompt.ptr(), before_key_len);
        MEMCPY(new_str + before_key_len, content.ptr(), content.length());
        MEMCPY(new_str + before_key_len + content.length(), key_pos + key.length(), after_key_len);
        result.assign_ptr(new_str, static_cast<ObString::obstr_size_t>(new_len));
      }
    }
  }
  return ret;
}

int ObAIFuncJsonUtils::transform_array_to_json_array(ObIAllocator &allocator, ObArray<ObString> &contents, ObJsonArray *&array)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(array = OB_NEWx(ObJsonArray, &allocator, &allocator))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
      ObString content = contents[i];
      ObJsonString *j_str = OB_NEWx(ObJsonString, &allocator, content);
      if (OB_ISNULL(j_str)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        array->append(j_str);
      }
    }
  }
  return ret;
}

int ObAIFuncUtils::get_complete_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIComplete *&complete_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::ALIYUN) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::DEEPSEEK)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)
      || ob_provider_check(provider, ObAIFuncProviderUtils::HUNYUAN)) {
    complete_provider = OB_NEWx(ObOpenAIUtils::ObOpenAIComplete, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    complete_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeComplete, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OLLAMA)) {
    complete_provider = OB_NEWx(ObOllamaUtils::ObOllamaComplete, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(complete_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::get_embed_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIEmbed *&embed_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::ALIYUN)
      || ob_provider_check(provider, ObAIFuncProviderUtils::HUNYUAN)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)) {
    embed_provider = OB_NEWx(ObOpenAIUtils::ObOpenAIEmbed, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    embed_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeEmbed, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OLLAMA)) {
    embed_provider = OB_NEWx(ObOllamaUtils::ObOllamaEmbed, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(embed_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::get_rerank_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIRerank *&rerank_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)
          || ob_provider_check(provider, ObAIFuncProviderUtils::COHERE)) {
    rerank_provider = OB_NEWx(ObSiliconflowUtils::ObSiliconflowRerank, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    rerank_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeRerank, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "rerank support siliconflow, cohere and ailiyun-dashscope, this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(rerank_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_completion(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info is null");
  } else if (!is_completion_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_dense_embedding(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info is null");
  } else if (!is_dense_embedding_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_rerank(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info is null");
  } else if (!is_rerank_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info type is not rerank");
  }
  return ret;
}

int ObAIFuncUtils::set_string_result(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res, ObString &res_str)
{
  int ret = OB_SUCCESS;
  ObTextStringDatumResult text_result(expr.datum_meta_.type_, &expr, &ctx, &res);
  int64_t res_len = res_str.length();
  if (OB_FAIL(text_result.init(res_len))) {
  } else if (OB_FAIL(text_result.append(res_str))) {
  } else {
    text_result.set_result();
  }
  return ret;
}

int ObAIFuncUtils::get_ai_func_info(ObIAllocator &allocator, const ObString &model_id,
                                    share::schema::ObSchemaGetterGuard &guard,
                                    ObAIFuncExprInfo *&info)
{
  int ret = OB_SUCCESS;
  if (model_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, model_id is empty");
  } else {
    ObAIFuncExprInfo *info_obj = OB_NEWx(ObAIFuncExprInfo, (&allocator), allocator, T_FUN_SYS_AI_COMPLETE);
    if (OB_ISNULL(info_obj)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(info_obj->init(allocator, model_id, guard))) {
    } else {
      info = info_obj;
    }
  }
  return ret;
}

bool ObAIFuncUtils::is_provider_support_base64(const ObString &provider)
{
  return ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW);
}

int ObAIFuncUtils::decode_base64_embedding_array(const ObIJsonBase &embedding_jbase,
                                                 ObIAllocator &allocator,
                                                 const int64_t dimension,
                                                 float *&vector)
{
  int ret = OB_SUCCESS;
  if (embedding_jbase.json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *encoded_embedding = embedding_jbase.get_data();
    uint64_t encoded_embedding_len = embedding_jbase.get_data_length();
    uint64_t decoded_buf_len = ObBase64Encoder::needed_decoded_length(encoded_embedding_len);
    uint8_t *decoded_buf = nullptr;
    int64_t pos = 0;
    if (decoded_buf_len <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(decoded_buf = static_cast<uint8_t *>(allocator.alloc(decoded_buf_len)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(ObBase64Encoder::decode(encoded_embedding, encoded_embedding_len,
                                               decoded_buf, decoded_buf_len, pos))) {
    } else if (pos != dimension * sizeof(float)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      vector = reinterpret_cast<float *>(decoded_buf);
    }
  }
  return ret;
}

int ObAIFuncUtils::decode_float_embedding_array(const ObIJsonBase &embedding_jbase,
                                                ObIAllocator &allocator,
                                                share::ObJsonReaderHelper &json_reader,
                                                const int64_t dimension,
                                                float *&vector)
{
  int ret = OB_SUCCESS;
  float *tmp_vector = nullptr;
  if (!share::ObJsonHelper::is_array_type(&embedding_jbase)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    uint64_t embedding_size = json_reader.get_array_size(&embedding_jbase);
    if (embedding_size != dimension) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      tmp_vector = static_cast<float*>(allocator.alloc(dimension * sizeof(float)));
      if (OB_ISNULL(tmp_vector)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (uint64_t i = 0; i < dimension && OB_SUCC(ret); i++) {
          ObIJsonBase *value = nullptr;
          if (OB_FAIL(json_reader.get_array_element(&embedding_jbase, i, value))) {
          } else if (!share::ObJsonHelper::is_number_type(value)) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            float f_value = 0.0;
            if (OB_FAIL(json_reader.get_float_value(value, f_value))) {
            } else {
              tmp_vector[i] = f_value;
            }
          }
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    vector = tmp_vector;
  }
  return ret;
}

int ObAIFuncUtils::get_ai_func_info(ObIAllocator &allocator, const ObString &model_id, ObAIFuncExprInfo *&info)
{
  int ret = OB_SUCCESS;
  share::schema::ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  share::schema::ObSchemaGetterGuard guard;
  
  if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(get_ai_func_info(allocator, model_id, guard, info))) {
  }
  return ret;
}

// the content of the model response is empty, the result of the ai function is invalid then
static bool is_empty_ai_result(ObIJsonBase *result_base)
{
  bool is_empty = false;
  if (OB_ISNULL(result_base) || result_base->json_type() == ObJsonNodeType::J_NULL) {
    is_empty = true;
  } else if (result_base->json_type() == ObJsonNodeType::J_STRING) {
    is_empty = static_cast<ObJsonString *>(result_base)->get_str().empty();
  }
  return is_empty;
}

int ObAIFuncModel::call_completion(ObString &prompt, ObJsonObject *config, ObString &result)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIComplete *complete_provider = nullptr;
  ObString prompt_str;
  ObString result_str;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  bool parse_failed = false;
  if (!is_completion_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  } else if (OB_FAIL(ObAIFuncUtils::get_complete_provider(*allocator_, endpoint_info_.get_provider(), complete_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(complete_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(complete_provider->get_body(*allocator_, request_model_name, prompt_str, prompt, config, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(complete_provider->parse_output(*allocator_, response, result_base))) {
    parse_failed = true;
  } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(*allocator_, result_base, result_str))) {
  } else if (result_str.empty() || is_empty_ai_result(result_base)) {
    // the model service returns an empty content, the result is invalid
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "ai_complete, the response content from the model is empty");
  } else {
    result = result_str;
  }
  if (parse_failed) {
    ObString response_str;
    if (OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(*allocator_, response, response_str)) {
      char http_message_str[1024];
      snprintf(http_message_str, sizeof(http_message_str), "unexpected http message: %s", response_str.ptr());
      ObString ob_http_message_str(http_message_str);
      FORWARD_USER_ERROR(ret, ob_http_message_str.ptr());
    } else {
      FORWARD_USER_ERROR(ret, "unexpected http message");
    }
  } 
  return ret;
}

int ObAIFuncModel::call_completion_with_system(const ObString &system,
                                                const ObString &content,
                                                ObJsonObject *config,
                                                ObString &result)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIComplete *complete_provider = nullptr;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  ObString system_message = system;
  ObString user_content = content;
  bool parse_failed = false;
  if (!is_completion_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  } else if (OB_FAIL(ObAIFuncUtils::get_complete_provider(*allocator_, endpoint_info_.get_provider(),
                                                          complete_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(complete_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(complete_provider->get_body(*allocator_, request_model_name, system_message,
                                                 user_content, config, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(complete_provider->parse_output(*allocator_, response, result_base))) {
    parse_failed = true;
  } else if (is_empty_ai_result(result_base)) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "ai_complete, the response content from the model is empty");
  } else if (ObJsonNodeType::J_STRING == result_base->json_type()) {
    result = static_cast<ObJsonString *>(result_base)->get_str();
  } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(*allocator_, result_base, result))) {
  }
  if (parse_failed) {
    ObString response_str;
    if (OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(*allocator_, response, response_str)) {
      char http_message_str[1024];
      snprintf(http_message_str, sizeof(http_message_str), "unexpected http message: %s", response_str.ptr());
      ObString ob_http_message_str(http_message_str);
      FORWARD_USER_ERROR(ret, ob_http_message_str.ptr());
    } else {
      FORWARD_USER_ERROR(ret, "unexpected http message");
    }
  }
  return ret;
}

int ObAIFuncModel::call_completion_vector(ObArray<ObString> &prompts, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObArray<ObJsonObject *> body_array;
  ObArray<ObJsonObject *> response_array;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIComplete *complete_provider = nullptr;
  ObString prompt_str;
  ObString result_str;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_completion_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  } else if (OB_FAIL(ObAIFuncUtils::get_complete_provider(*allocator_, endpoint_info_.get_provider(), complete_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(complete_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < prompts.count(); i++) {
      ObString prompt = prompts[i];
      if (OB_FAIL(complete_provider->get_body(*allocator_, request_model_name, prompt_str, prompt, config, body))) {
      } else if (OB_FAIL(body_array.push_back(body))) {
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(client.send_post_batch(*allocator_, endpoint_info_.get_url(), headers, body_array, response_array))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < response_array.count(); i++) {
      ObJsonObject *response = response_array[i];
      ObStringBuffer result_buf(allocator_);
      if (OB_FAIL(complete_provider->parse_output(*allocator_, response, result_base))) {
      } else if (OB_FAIL(result_base->print(result_buf, 0))) {
      } else {
        results.push_back(result_buf.string());
      }
    }
  }
  return ret;
}

int ObAIFuncModel::call_dense_embedding(ObString &content, ObJsonObject *config, ObString &result)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> contents;
  ObArray<ObString> results;
  if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
  } else if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (OB_FAIL(contents.push_back(content))) {
  } else if (OB_FAIL(call_dense_embedding_vector_v2(contents, config, results))) {
  } else if (results.count() != 1) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result = results[0];
  }
  return ret;
}

int ObAIFuncModel::call_dense_embedding_vector(ObArray<ObString> &contents, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObArray<ObJsonObject *> body_array;
  ObArray<ObJsonObject *> response_array;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIEmbed *embed_provider = nullptr;
  ObAIFuncClient client;
  ObString request_model_name = get_request_model_name();
  ObString unencrypted_access_key;
  if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (OB_FAIL(ObAIFuncUtils::get_embed_provider(*allocator_, endpoint_info_.get_provider(), embed_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(embed_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
      ObString content = contents[i];
      ObArray<ObString> content_array;
      if (OB_FAIL(content_array.push_back(content))) {
      } else if (OB_FAIL(embed_provider->get_body(*allocator_, request_model_name, content_array, config, body))) {
      } else if (OB_FAIL(body_array.push_back(body))) {
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(client.send_post_batch(*allocator_, endpoint_info_.get_url(), headers, body_array, response_array))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < response_array.count(); i++) {
      ObJsonObject *response = response_array[i];
      ObStringBuffer result_buf(allocator_);
      if (OB_FAIL(embed_provider->parse_output(*allocator_, response, result_base))) {
      } else if (OB_FAIL(result_base->print(result_buf, 0))) {
      } else {
        results.push_back(result_buf.string());
      }
    }
  }
  return ret;
}


// check the dimension of the embedding which is returned by the model service.
// the service may return the embedding as a json array of the float values, or
// as a base64 encoded string
static int check_embedding_dimension(ObIJsonBase *j_base, const int64_t dimension)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(j_base)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (j_base->json_type() == ObJsonNodeType::J_ARRAY) {
    if (static_cast<ObJsonArray *>(j_base)->element_count() != dimension) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, result dimension is not equal to dimension");
    }
  } else if (j_base->json_type() == ObJsonNodeType::J_STRING) {
    ObString base64_str = static_cast<ObJsonString *>(j_base)->get_str();
    int64_t encoded_len = base64_str.length();
    int64_t decoded_len = 0;
    if (encoded_len > 0 && encoded_len % 4 == 0) {
      decoded_len = (encoded_len / 4) * 3;
      if (base64_str.ptr()[encoded_len - 1] == '=') {
        decoded_len -= 1;
        if (base64_str.ptr()[encoded_len - 2] == '=') {
          decoded_len -= 1;
        }
      }
    }
    if (decoded_len != dimension * static_cast<int64_t>(sizeof(float))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "ai_embed, result dimension is not equal to dimension");
    }
  } else {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, the result of the model service is invalid");
  }
  return ret;
}

int ObAIFuncModel::call_dense_embedding_vector_v2(ObArray<ObString> &content, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIEmbed *embed_provider = nullptr;
  ObString result_str;
  ObAIFuncClient client;
  int64_t dimension = 0;
  if (OB_NOT_NULL(config)) {
    ObJsonNode *dimension_node = config->get_value("dimensions");
    if (OB_ISNULL(dimension_node)) {
      // do nothing
    } else if (dimension_node->json_type() != ObJsonNodeType::J_INT) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      dimension = static_cast<ObJsonInt *>(dimension_node)->get_int();
    }
  }
  
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (OB_FAIL(ObAIFuncUtils::get_embed_provider(*allocator_, endpoint_info_.get_provider(), embed_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(embed_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(embed_provider->get_body(*allocator_, request_model_name, content, config, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(embed_provider->parse_output(*allocator_, response, result_base))) {
  } else {
    ObJsonArray *result_array = static_cast<ObJsonArray *>(result_base);
    int64_t count = result_array->element_count();
    if (content.count() != count) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        ObIJsonBase *j_base = result_array->get_value(i);
        if (OB_ISNULL(j_base)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (dimension > 0 && OB_FAIL(check_embedding_dimension(j_base, dimension))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(*allocator_, j_base, result_str))) {
        } else {
          results.push_back(result_str);
        } 
      }
    }
  }
  if (ret == OB_INVALID_DATA) {
    ObString response_str;
    if (OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(*allocator_, response, response_str)) {
      char http_message_str[1024];
      snprintf(http_message_str, sizeof(http_message_str), "unexpected http message: %s", response_str.ptr());
      ObString ob_http_message_str(http_message_str);
      FORWARD_USER_ERROR(ret, ob_http_message_str.ptr());
    } else {
      FORWARD_USER_ERROR(ret, "unexpected http message");
    }
  }
  return ret;
}

int ObAIFuncModel::call_rerank(ObString &query, ObJsonArray *contents, ObJsonArray *&results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIRerank *rerank_provider = nullptr;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_rerank_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info type is not rerank");
  } else if (OB_FAIL(ObAIFuncUtils::get_rerank_provider(*allocator_, endpoint_info_.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(rerank_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(rerank_provider->get_body(*allocator_, request_model_name, query, contents, nullptr, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(rerank_provider->parse_output(*allocator_, response, result_base))) {
  } else {
    results = static_cast<ObJsonArray *>(result_base);
  }

  if (ret == OB_INVALID_DATA) {
    ObString response_str;
    if (OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(*allocator_, response, response_str)) {
      char http_message_str[1024];
      snprintf(http_message_str, sizeof(http_message_str), "unexpected http message: %s", response_str.ptr());
      ObString ob_http_message_str(http_message_str);
      FORWARD_USER_ERROR(ret, ob_http_message_str.ptr());
    } else {
      FORWARD_USER_ERROR(ret, "unexpected http message");
    }
  } 
  return ret;
}

bool ObAIFuncJsonUtils::ob_is_json_array_all_str(ObJsonArray* json_array)
{
  bool is_all_str = true;
  if (OB_ISNULL(json_array)) {
    is_all_str = false;
  }
  for (int64_t i = 0; is_all_str && i < json_array->element_count(); i++) {
    ObJsonNode *node = json_array->get_value(i);
    if (OB_ISNULL(node)) {
      is_all_str = false;
    } else if (node->json_type() != ObJsonNodeType::J_STRING) {
      is_all_str = false;
    }
  }
  return is_all_str;
}

int ObAIFuncPromptObjectUtils::construct_prompt_object(ObIAllocator &allocator, ObString &template_str, ObJsonArray *args_array, ObJsonObject *&prompt_object)
{
  INIT_SUCC(ret);
  ObJsonObject *prompt_obj = NULL;
  ObJsonString *template_json_str = NULL;
  if (template_str.empty() || args_array == NULL) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, template_str, template_json_str))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, prompt_obj))) {
  } else if (OB_FAIL(prompt_obj->add(ObAIFuncPromptObjectUtils::prompt_template_key, template_json_str))) {
  } else if (OB_FAIL(prompt_obj->add(ObAIFuncPromptObjectUtils::prompt_args_key, args_array))) {
  }
  if (OB_SUCC(ret)) {
    prompt_object = prompt_obj;
  }
  return ret;
}

bool ObAIFuncPromptObjectUtils::is_valid_prompt_object(ObJsonObject *prompt_object)
{
  bool is_valid = true;
  ObJsonNode *template_node = NULL;
  ObJsonNode *args_node = NULL;
  if (OB_ISNULL(prompt_object)) {
    is_valid = false;
  } else if (prompt_object->element_count() != 2) {
    is_valid = false;
  } else if (OB_ISNULL(template_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_template_key)) ||
            (OB_ISNULL(args_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key)))) {
    is_valid = false;
  } else if (template_node->json_type() != ObJsonNodeType::J_STRING || args_node->json_type() != ObJsonNodeType::J_ARRAY) {
    is_valid = false;
  }
  if (is_valid) {
    ObJsonArray *args_array = static_cast<ObJsonArray *>(args_node);
    for (int64_t i = 0; is_valid && i < args_array->element_count(); i++) {
      ObJsonNode *node = args_array->get_value(i);
      if (OB_ISNULL(node)) {  
        is_valid = false;
      } else if (node->json_type() == ObJsonNodeType::J_NULL) {
        // the null argument is not supported
        is_valid = false;
      }
    }
  }
  return is_valid;
}

// find the placeholder in the template. the placeholder is the index of the argument,
// such as {0} and {1}. the is_placeholder parameter is set to false if the text is not
// a placeholder, such text is copied to the result directly
static int find_placeholder_in_template(const ObString &template_str,
                                        const int64_t pos,
                                        int64_t &index,
                                        int64_t &end_pos,
                                        bool &is_placeholder)
{
  int ret = OB_SUCCESS;
  const char *ptr = template_str.ptr();
  const int64_t len = template_str.length();
  index = -1;
  end_pos = pos;
  is_placeholder = false;
  if (OB_ISNULL(ptr) || pos >= len || ptr[pos] != '{') {
    // it is not a placeholder
  } else {
    int64_t j = pos + 1;
    const int64_t digit_begin = j;
    int64_t value = 0;
    bool overflow = false;
    while (j < len && ptr[j] >= '0' && ptr[j] <= '9') {
      if (value > (INT64_MAX - (ptr[j] - '0')) / 10) {
        overflow = true;
      } else {
        value = value * 10 + (ptr[j] - '0');
      }
      j++;
    }
    if (overflow) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid placeholder index");
    } else if (j > digit_begin && j < len && ptr[j] == '}') {
      index = value;
      end_pos = j;
      is_placeholder = true;
    }
    // else it is not a placeholder, the text is copied to the result directly
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(ObIAllocator &allocator, ObJsonObject* prompt_object, ObString& replaced_prompt_str)
{
  INIT_SUCC(ret);
  ObJsonString *template_json_str = NULL;
  ObJsonArray *args_array = NULL;
  ObString template_str;
  if (OB_ISNULL(prompt_object) ||
      OB_ISNULL(template_json_str = static_cast<ObJsonString *>(prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_template_key))) ||
      OB_ISNULL(args_array = static_cast<ObJsonArray *>(prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key))) ||
      OB_ISNULL(template_str = template_json_str->get_str()) ||
      (template_str.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const uint64_t args_count = args_array->element_count();
    // render every argument to the text which replaces the placeholder in the template
    ObArray<ObString> arg_values;
    for (uint64_t i = 0; OB_SUCC(ret) && i < args_count; i++) {
      ObJsonNode *arg_node = args_array->get_value(i);
      ObString arg_value;
      if (OB_ISNULL(arg_node)) {
        ret = OB_INVALID_ARGUMENT;
      } else if (arg_node->json_type() == ObJsonNodeType::J_STRING) {
        arg_value = static_cast<ObJsonString *>(arg_node)->get_str();
      } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(allocator, arg_node, arg_value))) {
        // do nothing
      }
      if (OB_SUCC(ret) && OB_FAIL(arg_values.push_back(arg_value))) {
        // do nothing
      }
    }
    int64_t result_len = 0;
    if (OB_SUCC(ret)) {
      // compute the length of the result, one argument may be used by several placeholders
      const int64_t template_len = template_str.length();
      for (int64_t i = 0; i < template_len && OB_SUCC(ret); i++) {
        int64_t index = -1;
        int64_t end_pos = i;
        bool is_placeholder = false;
        if (OB_FAIL(find_placeholder_in_template(template_str, i, index, end_pos, is_placeholder))) {
          // do nothing
        } else if (is_placeholder) {
          if (static_cast<uint64_t>(index) >= args_count) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid placeholder index");
          } else {
            result_len += arg_values[index].length();
            i = end_pos;
          }
        } else {
          result_len += 1;
        }
      }
    }
    char *result_buf = NULL;
    if (OB_SUCC(ret) && OB_ISNULL(result_buf = static_cast<char *>(allocator.alloc(result_len + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_SUCC(ret)) {
      int64_t result_pos = 0;
      const char *template_ptr = template_str.ptr();
      const int64_t template_len = template_str.length();
      for (int64_t i = 0; i < template_len && OB_SUCC(ret); i++) {
        int64_t index = -1;
        int64_t end_pos = i;
        bool is_placeholder = false;
        if (OB_FAIL(find_placeholder_in_template(template_str, i, index, end_pos, is_placeholder))) {
          // do nothing
        } else if (is_placeholder) {
          if (static_cast<uint64_t>(index) >= args_count) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid placeholder index");
          } else {
            ObString &arg_value = arg_values[index];
            if (result_pos + arg_value.length() > result_len) {
              ret = OB_BUF_NOT_ENOUGH;
            } else {
              MEMCPY(result_buf + result_pos, arg_value.ptr(), arg_value.length());
              result_pos += arg_value.length();
            }
          }
          i = end_pos;
        } else {
          result_buf[result_pos++] = template_ptr[i];
        }
      }
      if (OB_SUCC(ret)) {
        replaced_prompt_str.assign_ptr(result_buf, static_cast<int32_t>(result_pos));
      }
    }
  }
  return ret;
}

const ObString ObAIFuncModel::get_request_model_name()
{
  ObString request_model_name = info_.model_;
  if (!endpoint_info_.get_request_model_name().empty()) {
    request_model_name = endpoint_info_.get_request_model_name();
  }
  return request_model_name;
}

// encode the binary content to the base64 format
static int encode_base64_str(ObIAllocator &allocator, const ObString &binary_str, ObString &base64_str)
{
  int ret = OB_SUCCESS;
  const int64_t encoded_len = ObBase64Encoder::needed_encoded_length(binary_str.length());
  char *encoded_buf = static_cast<char *>(allocator.alloc(encoded_len + 1));
  int64_t pos = 0;
  if (OB_ISNULL(encoded_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObBase64Encoder::encode(reinterpret_cast<const uint8_t *>(binary_str.ptr()),
                                            binary_str.length(),
                                            encoded_buf,
                                            encoded_len + 1,
                                            pos))) {
  } else {
    base64_str.assign_ptr(encoded_buf, static_cast<int32_t>(pos));
  }
  return ret;
}

int ObAiFuncImageUtils::get_type_from_binary(ObIAllocator &allocator, const ObString &binary_image_str, ObImageType &image_type)
{
  int ret = OB_SUCCESS;
  const char *ptr = binary_image_str.ptr();
  const int64_t len = binary_image_str.length();
  image_type = IMAGE_TYPE_UNKNOWN;
  if (OB_ISNULL(ptr) || len < 3) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "ai_prompt: the image binary content is invalid");
  } else if (len >= 3 && 0xFF == (uint8_t)ptr[0] && 0xD8 == (uint8_t)ptr[1] && 0xFF == (uint8_t)ptr[2]) {
    image_type = IMAGE_TYPE_JPEG;
  } else if (len >= 8 && 0x89 == (uint8_t)ptr[0] && 'P' == ptr[1] && 'N' == ptr[2] && 'G' == ptr[3]
             && 0x0D == (uint8_t)ptr[4] && 0x0A == (uint8_t)ptr[5] && 0x1A == (uint8_t)ptr[6] && 0x0A == (uint8_t)ptr[7]) {
    image_type = IMAGE_TYPE_PNG;
  } else if (len >= 4 && 'G' == ptr[0] && 'I' == ptr[1] && 'F' == ptr[2] && '8' == ptr[3]) {
    image_type = IMAGE_TYPE_GIF;
  } else if (len >= 12 && 'R' == ptr[0] && 'I' == ptr[1] && 'F' == ptr[2] && 'F' == ptr[3]
             && 'W' == ptr[8] && 'E' == ptr[9] && 'B' == ptr[10] && 'P' == ptr[11]) {
    image_type = IMAGE_TYPE_WEBP;
  } else if (len >= 2 && 'B' == ptr[0] && 'M' == ptr[1]) {
    image_type = IMAGE_TYPE_BMP;
  } else if (len >= 4 && ((0x49 == (uint8_t)ptr[0] && 0x49 == (uint8_t)ptr[1] && 0x2A == (uint8_t)ptr[2] && 0x00 == (uint8_t)ptr[3])
                        || (0x4D == (uint8_t)ptr[0] && 0x4D == (uint8_t)ptr[1] && 0x00 == (uint8_t)ptr[2] && 0x2A == (uint8_t)ptr[3]))) {
    image_type = IMAGE_TYPE_TIFF;
  } else if (len >= 4 && 0x00 == (uint8_t)ptr[0] && 0x00 == (uint8_t)ptr[1] && 0x01 == (uint8_t)ptr[2] && 0x00 == (uint8_t)ptr[3]) {
    image_type = IMAGE_TYPE_ICO;
  } else if (len >= 4 && 'i' == ptr[0] && 'c' == ptr[1] && 'n' == ptr[2] && 's' == ptr[3]) {
    image_type = IMAGE_TYPE_ICNS;
  } else if (len >= 2 && 0x01 == (uint8_t)ptr[0] && (0xDA == (uint8_t)ptr[1] || 0x52 == (uint8_t)ptr[1])) {
    image_type = IMAGE_TYPE_SGI;
  } else {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "ai_prompt: the image type is not supported");
  }
  return ret;
}

const char *ObAiFuncImageUtils::get_image_type_str(ObImageType image_type)
{
  const char *image_type_str = "unknown";
  switch (image_type) {
    case IMAGE_TYPE_JPEG: image_type_str = "jpeg"; break;
    case IMAGE_TYPE_PNG: image_type_str = "png"; break;
    case IMAGE_TYPE_GIF: image_type_str = "gif"; break;
    case IMAGE_TYPE_WEBP: image_type_str = "webp"; break;
    case IMAGE_TYPE_BMP: image_type_str = "bmp"; break;
    case IMAGE_TYPE_TIFF: image_type_str = "tiff"; break;
    case IMAGE_TYPE_ICO: image_type_str = "ico"; break;
    case IMAGE_TYPE_DIB: image_type_str = "dib"; break;
    case IMAGE_TYPE_ICNS: image_type_str = "icns"; break;
    case IMAGE_TYPE_SGI: image_type_str = "sgi"; break;
    default: break;
  }
  return image_type_str;
}

int ObAiFuncImageUtils::get_base64_data_uri_from_binary(ObIAllocator &allocator, const ObString &binary_image_str, ObString &base64_data_uri)
{
  int ret = OB_SUCCESS;
  ObImageType image_type = IMAGE_TYPE_UNKNOWN;
  ObString base64_str;
  if (OB_FAIL(get_type_from_binary(allocator, binary_image_str, image_type))) {
  } else if (OB_FAIL(encode_base64_str(allocator, binary_image_str, base64_str))) {
  } else {
    const char *image_type_str = get_image_type_str(image_type);
    const int64_t prefix_len = strlen("data:image/") + strlen(image_type_str) + strlen(";base64,");
    char *data_uri_buf = static_cast<char *>(allocator.alloc(prefix_len + base64_str.length() + 1));
    if (OB_ISNULL(data_uri_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      snprintf(data_uri_buf, prefix_len + base64_str.length() + 1, "data:image/%s;base64,%.*s",
               image_type_str, static_cast<int32_t>(base64_str.length()), base64_str.ptr());
      base64_data_uri.assign_ptr(data_uri_buf, static_cast<int32_t>(strlen(data_uri_buf)));
    }
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::parse_template_arg_types(const ObString &template_str,
                                                         const int64_t arg_cnt,
                                                         ObIArray<PromptArgType> &arg_types,
                                                         bool *has_image)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(has_image)) {
    *has_image = false;
  }
  const char *ptr = template_str.ptr();
  const int64_t len = template_str.length();
  if (OB_ISNULL(ptr)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = 0; i < len && OB_SUCC(ret); i++) {
      if (ptr[i] != '{') {
        continue;
      }
      int64_t j = i + 1;
      bool is_image = false;
      if (j + 3 < len && 'i' == ptr[j] && 'm' == ptr[j + 1] && 'g' == ptr[j + 2] && '_' == ptr[j + 3]) {
        is_image = true;
        j += 4;
      }
      const int64_t digit_begin = j;
      int64_t index = 0;
      bool overflow = false;
      while (j < len && ptr[j] >= '0' && ptr[j] <= '9') {
        if (index > (INT64_MAX - (ptr[j] - '0')) / 10) {
          overflow = true;
        } else {
          index = index * 10 + (ptr[j] - '0');
        }
        j++;
      }
      if (overflow) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid placeholder index");
      } else if (j > digit_begin && j < len && ptr[j] == '}') {
        if (index < 0 || index >= arg_cnt || index >= arg_types.count()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: placeholder index out of range");
        } else if (arg_types.at(index) != PromptArgType::UNKNOWN) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: duplicated placeholder index");
        } else {
          arg_types.at(index) = is_image ? PromptArgType::IMAGE : PromptArgType::TEXT;
          if (is_image && OB_NOT_NULL(has_image)) {
            *has_image = true;
          }
        }
        i = j;
      }
      // else it is not a valid placeholder, the text is ignored
    }
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::build_image_arg_object(ObIAllocator &allocator,
                                                      const ObString &arg_str,
                                                      const ImageArgType image_arg_type,
                                                      ObJsonObject *&image_obj)
{
  int ret = OB_SUCCESS;
  ObAiFuncImageUtils::ObImageType image_type = ObAiFuncImageUtils::IMAGE_TYPE_UNKNOWN;
  if (ImageArgType::URL == image_arg_type) {
    const char *ptr = arg_str.ptr();
    const int64_t len = arg_str.length();
    bool is_http = (len >= 7 && 0 == memcmp(ptr, ObAIFuncPromptObjectUtils::HTTP_PREFIX, 7));
    bool is_https = (len >= 8 && 0 == memcmp(ptr, ObAIFuncPromptObjectUtils::HTTPS_PREFIX, 8));
    if (!is_http && !is_https) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt image url should start with http or https");
    }
  } else if (ImageArgType::BINARY == image_arg_type) {
    ret = ObAiFuncImageUtils::get_type_from_binary(allocator, arg_str, image_type);
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid image argument type");
  }
  if (OB_SUCC(ret)) {
    ObJsonObject *image_object = NULL;
    ObJsonString *type_str = NULL;
    ObString image_type_str("image");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, image_object))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, image_type_str, type_str))) {
    } else if (OB_FAIL(image_object->add("type", type_str))) {
    } else if (ImageArgType::URL == image_arg_type) {
      ObJsonString *url_str = NULL;
      ObString url_str_value = arg_str;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, url_str_value, url_str))) {
      } else if (OB_FAIL(image_object->add("url", url_str))) {
      } else {
        image_obj = image_object;
      }
    } else {
      // the binary image is encoded to the base64 format
      ObString base64_str;
      ObString format_str_value(ObAiFuncImageUtils::get_image_type_str(image_type));
      ObJsonString *format_str = NULL;
      ObJsonString *data_str = NULL;
      if (OB_FAIL(encode_base64_str(allocator, arg_str, base64_str))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, format_str_value, format_str))) {
      } else if (OB_FAIL(image_object->add("format", format_str))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, base64_str, data_str))) {
      } else if (OB_FAIL(image_object->add("data", data_str))) {
      } else {
        image_obj = image_object;
      }
    }
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::validate_binary_image_sizes(ObJsonObject *prompt_object, const int64_t max_image_size)
{
  int ret = OB_SUCCESS;
  ObJsonNode *args_node = NULL;
  if (OB_ISNULL(prompt_object)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(args_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key))) {
    // do nothing
  } else if (args_node->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *args_array = static_cast<ObJsonArray *>(args_node);
    for (int64_t i = 0; OB_SUCC(ret) && i < args_array->element_count(); i++) {
      ObJsonNode *arg_node = args_array->get_value(i);
      if (OB_ISNULL(arg_node) || arg_node->json_type() != ObJsonNodeType::J_OBJECT) {
        continue;
      }
      ObJsonNode *data_node = static_cast<ObJsonObject *>(arg_node)->get_value("data");
      if (OB_ISNULL(data_node) || data_node->json_type() != ObJsonNodeType::J_STRING) {
        continue;
      }
      ObString base64_str = static_cast<ObJsonString *>(data_node)->get_str();
      int64_t decoded_len = ObBase64Encoder::needed_decoded_length(base64_str.length());
      if (max_image_size > 0 && decoded_len > max_image_size) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: the size of the binary image is too large");
      }
    }
  }
  return ret;
}

ObAIModelConfigInfoManager::ObAIModelConfigInfoManager() : is_inited_(false) {}

ObAIModelConfigInfoManager::~ObAIModelConfigInfoManager() {}

ObAIModelConfigInfoManager &ObAIModelConfigInfoManager::get_instance()
{
  static ObAIModelConfigInfoManager instance;
  return instance;
}

int ObAIModelConfigInfoManager::init()
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard guard(lock_);
  ret = init_locked_();
  return ret;
}

int ObAIModelConfigInfoManager::init_locked_()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    if (OB_FAIL(register_default_model_configs_())) {
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

void ObAIModelConfigInfoManager::reset()
{
  common::ObSpinLockGuard guard(lock_);
  config_array_.reset();
  is_inited_ = false;
}

int ObAIModelConfigInfoManager::set_config(const ObString &provider,
                                            const ObString &model_name,
                                            const share::ObAIModelConfigItem &item,
                                            const bool overwrite)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard guard(lock_);
  ret = set_config_locked_(provider, model_name, item, overwrite);
  return ret;
}

int ObAIModelConfigInfoManager::set_config_locked_(const ObString &provider,
                                                    const ObString &model_name,
                                                    const share::ObAIModelConfigItem &item,
                                                    const bool overwrite)
{
  int ret = OB_SUCCESS;
  if (provider.empty() || model_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (provider.length() >= static_cast<int64_t>(sizeof(ModelConfigEntry::provider_))
             || model_name.length() >= static_cast<int64_t>(sizeof(ModelConfigEntry::model_name_))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai model config, the provider or the model name is too long");
  } else {
    bool found = false;
    for (int64_t i = 0; !found && i < config_array_.count(); i++) {
      if (ObString(config_array_.at(i).provider_).case_compare(provider) == 0
          && ObString(config_array_.at(i).model_name_).case_compare(model_name) == 0) {
        if (overwrite) {
          config_array_.at(i).item_ = item;
        }
        found = true;
      }
    }
    if (!found) {
      ModelConfigEntry entry;
      snprintf(entry.provider_, sizeof(entry.provider_), "%.*s",
               static_cast<int>(provider.length()), provider.ptr());
      snprintf(entry.model_name_, sizeof(entry.model_name_), "%.*s",
               static_cast<int>(model_name.length()), model_name.ptr());
      entry.item_ = item;
      if (OB_FAIL(config_array_.push_back(entry))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  return ret;
}

int ObAIModelConfigInfoManager::get_model_config(const ObString &provider,
                                                  const ObString &request_model_name,
                                                  share::ObAIModelConfigItem &item)
{
  int ret = OB_SUCCESS;
  if (provider.empty() || request_model_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    common::ObSpinLockGuard guard(lock_);
    if (OB_FAIL(init_locked_())) {
    } else {
      item = share::ObAIModelConfigItem();
      for (int64_t i = 0; i < config_array_.count(); i++) {
        if (ObString(config_array_.at(i).provider_).case_compare(provider) == 0
            && ObString(config_array_.at(i).model_name_).case_compare(request_model_name) == 0) {
          item = config_array_.at(i).item_;
          break;
        }
      }
    }
  }
  return ret;
}

int ObAIModelConfigInfoManager::get_config(const ObString &provider,
                                            const ObString &model_name,
                                            share::ObAIModelConfigItem &item)
{
  return get_model_config(provider, model_name, item);
}

int ObAIModelConfigInfoManager::register_default_model_configs_()
{
  int ret = OB_SUCCESS;
  share::ObAIModelConfigItem item;
  // the well known configs of the dashscope multimodal embedding models
  item.batch_size_ = 5;
  item.max_image_size_ = 5 * 1024 * 1024;
  if (OB_FAIL(set_config_locked_(ObString(ObAIFuncProviderUtils::DASHSCOPE),
                                 ObString("qwen3-vl-embedding"), item))) {
  } else {
    item.batch_size_ = 1;
    if (OB_FAIL(set_config_locked_(ObString(ObAIFuncProviderUtils::DASHSCOPE),
                                   ObString("qwen2.5-vl-embedding"), item))) {
    } else {
      item.max_image_size_ = 3 * 1024 * 1024;
      if (OB_FAIL(set_config_locked_(ObString(ObAIFuncProviderUtils::DASHSCOPE),
                                     ObString("multimodal-embedding-v1"), item))) {
      } else {
        item.batch_size_ = 16;
        if (OB_FAIL(set_config_locked_(ObString(ObAIFuncProviderUtils::DASHSCOPE),
                                       ObString("tongyi-embedding-vision-plus"), item))) {
        } else if (OB_FAIL(set_config_locked_(ObString(ObAIFuncProviderUtils::DASHSCOPE),
                                              ObString("tongyi-embedding-vision-flash"), item))) {
        }
      }
    }
  }
  return ret;
}
} // namespace common

namespace query
{

int ObAIModelResolver::resolve_model_name(
    common::ObIAllocator &allocator,
    const common::ObString &model_id,
    common::ObString &model_name)
{
  int ret = OB_SUCCESS;
  common::ObAIFuncExprInfo *info = nullptr;
  if (OB_FAIL(common::ObAIFuncUtils::get_ai_func_info(
          allocator, model_id, info))) {
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ob_write_string(allocator, info->model_, model_name))) {
  }
  return ret;
}

} // namespace query
} // namespace oceanbase
