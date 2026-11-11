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

#include <gtest/gtest.h>
#include "ob_expr_test_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

class ObOpenAIUtilsTest : public ::testing::Test
{
public:
  ObOpenAIUtilsTest();
  virtual ~ObOpenAIUtilsTest();
  virtual void SetUp();
  virtual void TearDown();

private:
  // disallow copy
  ObOpenAIUtilsTest(const ObOpenAIUtilsTest &other);
  ObOpenAIUtilsTest &operator=(const ObOpenAIUtilsTest &other);
};

ObOpenAIUtilsTest::ObOpenAIUtilsTest() {}

ObOpenAIUtilsTest::~ObOpenAIUtilsTest() {}

void ObOpenAIUtilsTest::SetUp() {}

void ObOpenAIUtilsTest::TearDown() {}

TEST_F(ObOpenAIUtilsTest, test_complete_get_header)
{
  ObArray<ObString> headers;
  ObString api_key("sk-1234567890");
  ObString authorization("Authorization: Bearer sk-1234567890");
  ObString content_type("Content-Type: application/json");
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete completion;
  ASSERT_EQ(OB_SUCCESS, completion.get_header(allocator, api_key, headers));
  ASSERT_EQ(2, headers.count());
  ASSERT_EQ(authorization, headers[0]);
  ASSERT_EQ(content_type, headers[1]);
}

TEST_F(ObOpenAIUtilsTest, test_complete_get_body)
{
  ObArenaAllocator allocator;
  ObString model("gpt-4o-mini");
  ObString prompt("You are a helpful assistant.");
  ObString content("oceanbase is a distributed database");
  ObOpenAIUtils::ObOpenAIComplete completion;
  ObJsonObject *body = nullptr;
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator, model, prompt, content, config, body));
  ASSERT_TRUE(NULL != body);
  ObJsonNode *model_node = body->get_value("model");
  ASSERT_TRUE(NULL != model_node);
  ObStringBuffer model_buf(&allocator);
  model_node->print(model_buf, 0);
  ASSERT_EQ(model, model_buf.string());

  ObJsonNode *messages_node = body->get_value("messages");
  ASSERT_TRUE(NULL != messages_node);
  ObJsonArray *messages_array = static_cast<ObJsonArray *>(messages_node);
  ASSERT_EQ(2, messages_array->element_count());

  ObJsonNode *message_node1 = messages_array->get_value(0);
  ObJsonObject *message_obj1 = static_cast<ObJsonObject *>(message_node1);
  ObJsonNode *prompt_node = message_obj1->get_value("content");
  ASSERT_TRUE(NULL != prompt_node);
  ObStringBuffer prompt_buf(&allocator);
  prompt_node->print(prompt_buf, 0);
  ASSERT_EQ(prompt, prompt_buf.string());

  ObJsonNode *message_node2 = messages_array->get_value(1);
  ObJsonObject *message_obj2 = static_cast<ObJsonObject *>(message_node2);
  ObJsonNode *content_node = message_obj2->get_value("content");
  ASSERT_TRUE(NULL != content_node);
  ObStringBuffer content_buf(&allocator);
  content_node->print(content_buf, 0);
  ASSERT_EQ(content, content_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_complete_parse_output)
{
  ObArenaAllocator allocator;
  ObString content("天气真好。");
  ObString response(
      "{"
      "\"choices\": [ "
          "{"
              "\"message\": {"
                  "\"role\": \"assistant\","
                  "\"content\": \"天气真好。\""
              "},"
              "\"finish_reason\": \"stop\","
              "\"index\": 0,"
              "\"logprobs\": null"
          "}"
      "],"
      "\"object\": \"chat.completion\","
      "\"created\": 1735120033,"
      "\"model\": \"qwen-plus\","
      "\"id\": \"chatcmpl-6ada9ed2-7f33-9de2-8bb0-78bd4035025a\""
      "}"
  );
  ObOpenAIUtils::ObOpenAIComplete completion;
  ObIJsonBase *j_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  ObJsonObject *http_response = static_cast<ObJsonObject *>(j_base);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, completion.parse_output(allocator, http_response, result));
  ASSERT_TRUE(NULL != result);
  ObStringBuffer content_buf(&allocator);
  result->print(content_buf, 0);
  ASSERT_EQ(content, content_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_complete_parse_output_empty)
{
  ObArenaAllocator allocator;
  ObString response(
      "{"
      "\"choices\": [ "
          "{"
              "\"message\": {"
                  "\"role\": \"assistant\""
              "},"
              "\"finish_reason\": \"stop\","
              "\"index\": 0,"
              "\"logprobs\": null"
          "}"
      "],"
      "\"object\": \"chat.completion\","
      "\"created\": 1735120033,"
      "\"model\": \"qwen-plus\","
      "\"id\": \"chatcmpl-6ada9ed2-7f33-9de2-8bb0-78bd4035025a\""
      "}"
  );
  ObOpenAIUtils::ObOpenAIComplete completion;
  ObIJsonBase *j_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  ObJsonObject *http_response = static_cast<ObJsonObject *>(j_base);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_INVALID_DATA, completion.parse_output(allocator, http_response, result));
}

TEST_F(ObOpenAIUtilsTest, test_embedding_get_header)
{
  ObArenaAllocator allocator;
  ObString api_key("sk-1234567890");
  ObString authorization("Authorization: Bearer sk-1234567890");
  ObString content_type("Content-Type: application/json");
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, embedding.get_header(allocator, api_key, headers));
  ASSERT_EQ(2, headers.count());
  ASSERT_EQ(authorization, headers[0]);
  ASSERT_EQ(content_type, headers[1]);
}

TEST_F(ObOpenAIUtilsTest, test_embedding_get_body)
{
  ObArenaAllocator allocator;
  ObString model("text-embedding-3-small");
  ObString input("oceanbase is a distributed database");
  ObArray<ObString> input_array;
  input_array.push_back(input);
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObJsonObject *body = nullptr;
  ObJsonObject *config = nullptr;
  ObString input_type("text");
  ASSERT_EQ(OB_SUCCESS, embedding.get_body(allocator, model, input_array, config, input_type, body));
  ASSERT_TRUE(NULL != body);

  ObJsonNode *model_node = body->get_value("model");
  ASSERT_TRUE(NULL != model_node);
  ObStringBuffer model_buf(&allocator);
  model_node->print(model_buf, 0);
  ASSERT_EQ(model, model_buf.string());

  ObJsonNode *input_node = body->get_value("input");
  ASSERT_TRUE(NULL != input_node);
  ObJsonArray *input_array_node = static_cast<ObJsonArray *>(input_node);
  ASSERT_EQ(1, input_array_node->element_count());
  ObJsonNode *input_node2 = input_array_node->get_value(0);
  ASSERT_TRUE(NULL != input_node2);
  ObStringBuffer input_buf(&allocator);
  input_node2->print(input_buf, 0);
  ASSERT_EQ(input, input_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output)
{
  ObArenaAllocator allocator;
  ObString content("[0.0023064255, -0.009327292, -0.0028842222]");
  ObString response(
      "{"
          "\"data\": ["
              "{"
                  "\"embedding\": [0.0023064255, -0.009327292, -0.0028842222],"
                  "\"index\": 0,"
                  "\"object\": \"embedding\""
              "}"
          "],"
          "\"model\":\"text-embedding-v4\","
          "\"object\":\"list\","
          "\"usage\":{\"prompt_tokens\":23,\"total_tokens\":23},"
          "\"id\":\"f62c2ae7-0906-9758-ab34-47c5764f07e2\""
          "}"
  );
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObIJsonBase *j_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  ObJsonObject *http_response = static_cast<ObJsonObject *>(j_base);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator, http_response, result));

  ObJsonArray *embeddings_array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(1, embeddings_array->element_count());

  ObJsonNode *embedding_node = embeddings_array->get_value(0);
  ObJsonArray *embedding_array = static_cast<ObJsonArray *>(embedding_node);
  ObStringBuffer embedding_buf(&allocator);
  embedding_array->print(embedding_buf, 0);
  ASSERT_EQ(content, embedding_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output_empty)
{
  ObArenaAllocator allocator;
  ObString response(
      "{"
          "\"data\": ["
              "{"
                  "\"index\": 0,"
                  "\"object\": \"embedding\""
              "}"
          "],"
          "\"model\":\"text-embedding-v4\","
          "\"object\":\"list\","
          "\"usage\":{\"prompt_tokens\":23,\"total_tokens\":23},"
          "\"id\":\"f62c2ae7-0906-9758-ab34-47c5764f07e2\""
          "}"
  );
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObIJsonBase *j_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  ObJsonObject *http_response = static_cast<ObJsonObject *>(j_base);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_INVALID_DATA, embedding.parse_output(allocator, http_response, result));
}

TEST_F(ObOpenAIUtilsTest, test_model_config_manager_singleton_and_init)
{
  ObAIModelConfigInfoManager &manager = ObAIModelConfigInfoManager::get_instance();
  // the singleton instance should be stable
  ASSERT_EQ(&manager, &ObAIModelConfigInfoManager::get_instance());

  // reset and re-init should both succeed
  manager.reset();
  ASSERT_EQ(OB_SUCCESS, manager.init());
  ASSERT_GT(manager.size(), 0);
  // the repeated init should be a no-op and still succeed
  ASSERT_EQ(OB_SUCCESS, manager.init());
}

TEST_F(ObOpenAIUtilsTest, test_model_config_manager_get_model_config_positive)
{
  ObAIModelConfigInfoManager &manager = ObAIModelConfigInfoManager::get_instance();
  manager.reset();

  // the known model config should be found through the lazy init path
  oceanbase::share::ObAIModelConfigItem item;
  ASSERT_EQ(OB_SUCCESS, manager.get_model_config(ObAIFuncProviderUtils::DASHSCOPE,
                                                 ObString("qwen3-vl-embedding"),
                                                 item));
  ASSERT_EQ(5, item.batch_size_);
  ASSERT_EQ(5 * 1024 * 1024, item.max_image_size_);

  ASSERT_EQ(OB_SUCCESS, manager.get_model_config(ObAIFuncProviderUtils::DASHSCOPE,
                                                 ObString("qwen2.5-vl-embedding"),
                                                 item));
  ASSERT_EQ(1, item.batch_size_);
  ASSERT_EQ(5 * 1024 * 1024, item.max_image_size_);

  ASSERT_EQ(OB_SUCCESS, manager.get_model_config(ObAIFuncProviderUtils::DASHSCOPE,
                                                 ObString("multimodal-embedding-v1"),
                                                 item));
  ASSERT_EQ(1, item.batch_size_);
  ASSERT_EQ(3 * 1024 * 1024, item.max_image_size_);
}

TEST_F(ObOpenAIUtilsTest, test_model_config_manager_get_model_config_negative)
{
  ObAIModelConfigInfoManager &manager = ObAIModelConfigInfoManager::get_instance();
  manager.reset();
  oceanbase::share::ObAIModelConfigItem item;

  // the unknown model falls back to the default item
  ASSERT_EQ(OB_SUCCESS, manager.get_model_config(ObAIFuncProviderUtils::DASHSCOPE,
                                                 ObString("model-not-exists"),
                                                 item));
  ASSERT_EQ(0, item.batch_size_);
  ASSERT_EQ(0, item.max_image_size_);
  // the unknown provider behaves in the same way
  ASSERT_EQ(OB_SUCCESS, manager.get_model_config(ObString("UNKNOWN-PROVIDER"),
                                                 ObString("qwen3-vl-embedding"),
                                                 item));
  ASSERT_EQ(0, item.batch_size_);
  ASSERT_EQ(0, item.max_image_size_);
  // the invalid input should fail fast
  ASSERT_EQ(OB_INVALID_ARGUMENT, manager.get_model_config(ObString(),
                                                          ObString("qwen3-vl-embedding"),
                                                          item));
  ASSERT_EQ(OB_INVALID_ARGUMENT, manager.get_model_config(ObAIFuncProviderUtils::DASHSCOPE,
                                                          ObString(),
                                                          item));
}

TEST_F(ObOpenAIUtilsTest, test_model_config_manager_get_config_negative)
{
  ObAIModelConfigInfoManager &manager = ObAIModelConfigInfoManager::get_instance();
  manager.reset();
  oceanbase::share::ObAIModelConfigItem item;

  // get_config has the same argument checks
  ASSERT_EQ(OB_INVALID_ARGUMENT, manager.get_config(ObString(),
                                                    ObString("qwen3-vl-embedding"),
                                                    item));
  ASSERT_EQ(OB_INVALID_ARGUMENT, manager.get_config(ObAIFuncProviderUtils::DASHSCOPE,
                                                    ObString(),
                                                    item));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_complete_request)
{
  ObArenaAllocator allocator;
  // the access key of the ollama endpoint may be empty, only the content type is sent then
  ObString empty_api_key;
  ObArray<ObString> headers;
  ObOllamaUtils::ObOllamaComplete completion;
  ASSERT_EQ(OB_SUCCESS, completion.get_header(allocator, empty_api_key, headers));
  ASSERT_EQ(1, headers.count());
  ASSERT_EQ(ObString("Content-Type: application/json"), headers[0]);

  // the native completion protocol of the ollama service is not streamed
  ObString model("qwen2.5");
  ObString prompt("You are a helpful assistant.");
  ObString content("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator, model, prompt, content, nullptr, body));
  ASSERT_TRUE(NULL != body);
  ObJsonNode *model_node = body->get_value("model");
  ASSERT_TRUE(NULL != model_node);
  ObStringBuffer model_buf(&allocator);
  ASSERT_EQ(OB_SUCCESS, model_node->print(model_buf, 0));
  ASSERT_EQ(model, model_buf.string());
  ObJsonNode *prompt_node = body->get_value("prompt");
  ASSERT_TRUE(NULL != prompt_node);
  ObStringBuffer prompt_buf(&allocator);
  ASSERT_EQ(OB_SUCCESS, prompt_node->print(prompt_buf, 0));
  ASSERT_EQ(content, prompt_buf.string());
  ObJsonNode *stream_node = body->get_value("stream");
  ASSERT_TRUE(NULL != stream_node);
  ASSERT_EQ(ObJsonNodeType::J_BOOLEAN, stream_node->json_type());
  ASSERT_FALSE(static_cast<ObJsonBoolean *>(stream_node)->value());

  // the members of the config are merged into the body, except the managed ones
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator,
      ObString("{\"temperature\":0.2,\"stream\":true}"), config));
  ASSERT_EQ(OB_SUCCESS, completion.get_body(allocator, model, prompt, content, config, body));
  ASSERT_TRUE(NULL != body->get_value("temperature"));
  ASSERT_FALSE(static_cast<ObJsonBoolean *>(body->get_value("stream"))->value());
}

TEST_F(ObOpenAIUtilsTest, test_ollama_complete_parse_output)
{
  ObArenaAllocator allocator;
  ObString response("{\"model\":\"qwen2.5\",\"response\":\"天气真好。\",\"done\":true}");
  ObIJsonBase *j_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  ObOllamaUtils::ObOllamaComplete completion;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, completion.parse_output(allocator, static_cast<ObJsonObject *>(j_base), result));
  ASSERT_TRUE(NULL != result);
  ObStringBuffer result_buf(&allocator);
  ASSERT_EQ(OB_SUCCESS, result->print(result_buf, 0));
  ASSERT_EQ(ObString("天气真好。"), result_buf.string());

  // the response without the content is invalid
  ObString bad_response("{\"model\":\"qwen2.5\",\"done\":true}");
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, bad_response,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  result = nullptr;
  ASSERT_NE(OB_SUCCESS, completion.parse_output(allocator, static_cast<ObJsonObject *>(j_base), result));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_embed_request_and_response)
{
  ObArenaAllocator allocator;
  ObString empty_api_key;
  ObArray<ObString> headers;
  ObOllamaUtils::ObOllamaEmbed embedding;
  ASSERT_EQ(OB_SUCCESS, embedding.get_header(allocator, empty_api_key, headers));
  ASSERT_EQ(1, headers.count());

  ObString model("nomic-embed-text");
  ObArray<ObString> contents;
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("hello")));
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("world")));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.get_body(allocator, model, contents, nullptr, body));
  ASSERT_TRUE(NULL != body);
  ObJsonNode *input_node = body->get_value("input");
  ASSERT_TRUE(NULL != input_node);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, input_node->json_type());
  ASSERT_EQ(2, static_cast<ObJsonArray *>(input_node)->element_count());

  // {"model":"nomic-embed-text","embeddings":[[0.1,0.2],[0.3,0.4]]}
  ObString response("{\"model\":\"nomic-embed-text\",\"embeddings\":[[0.1,0.2],[0.3,0.4]]}");
  ObIJsonBase *j_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator, static_cast<ObJsonObject *>(j_base), result));
  ASSERT_TRUE(NULL != result);
  ObJsonArray *result_array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, result_array->element_count());
  ObStringBuffer embedding_buf(&allocator);
  ASSERT_EQ(OB_SUCCESS, result_array->get_value(0)->print(embedding_buf, 0));
  ASSERT_EQ(ObString("[0.1, 0.2]"), embedding_buf.string());
}

TEST_F(ObOpenAIUtilsTest, test_embedding_parse_output_out_of_order)
{
  ObArenaAllocator allocator;
  // the model service returns the embeddings in a different order than the inputs,
  // the "index" of every element restores the order of the inputs
  ObString response(
      "{"
          "\"data\": ["
              "{\"embedding\": [0.2, 0.2], \"index\": 1, \"object\": \"embedding\"},"
              "{\"embedding\": [0.1, 0.1], \"index\": 0, \"object\": \"embedding\"}"
          "],"
          "\"model\":\"text-embedding-v1\","
          "\"object\":\"list\""
      "}"
  );
  ObIJsonBase *j_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, response,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  ObOpenAIUtils::ObOpenAIEmbed embedding;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator, static_cast<ObJsonObject *>(j_base), result));
  ASSERT_TRUE(NULL != result);
  ObJsonArray *result_array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, result_array->element_count());
  ObStringBuffer first_buf(&allocator);
  ASSERT_EQ(OB_SUCCESS, result_array->get_value(0)->print(first_buf, 0));
  ASSERT_EQ(ObString("[0.1, 0.1]"), first_buf.string());
  ObStringBuffer second_buf(&allocator);
  ASSERT_EQ(OB_SUCCESS, result_array->get_value(1)->print(second_buf, 0));
  ASSERT_EQ(ObString("[0.2, 0.2]"), second_buf.string());

  // the index of the embedding is not usable, the order of the response is kept
  ObString no_index_response(
      "{\"data\": [{\"embedding\": [0.1]}, {\"embedding\": [0.2]}]}"
  );
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, no_index_response,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base));
  result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding.parse_output(allocator, static_cast<ObJsonObject *>(j_base), result));
  ObJsonArray *no_index_array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, no_index_array->element_count());
  ObStringBuffer no_index_buf(&allocator);
  ASSERT_EQ(OB_SUCCESS, no_index_array->get_value(0)->print(no_index_buf, 0));
  ASSERT_EQ(ObString("[0.1]"), no_index_buf.string());
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
