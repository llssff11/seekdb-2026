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

#ifndef OB_AI_FUNC_UTILS_H_
#define OB_AI_FUNC_UTILS_H_

#include "ob_ai_func.h"
#include "share/json/ob_json_helper.h"
#include "lib/encode/ob_base64_encode.h"
#include "lib/hash_func/murmur_hash.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/container/ob_array.h"

namespace oceanbase 
{
namespace share 
{
/**
 * @brief Static configuration of one ai model, such as the max batch size and
 *        the max size of one image which can be sent to the model service.
 */
struct ObAIModelConfigItem
{
  ObAIModelConfigItem() : batch_size_(0), max_image_size_(0) {}
  int64_t batch_size_;
  int64_t max_image_size_;
};
} // namespace share

namespace common 
{

class ObOpenAIUtils 
{
public:
  class ObOpenAIComplete : public ObAIFuncIComplete 
  {
  public:
    ObOpenAIComplete() {}
    virtual ~ObOpenAIComplete() {}
    virtual int get_header(common::ObIAllocator &allocator,
                           common::ObString &api_key,
                           common::ObArray<ObString> &headers) override;
    virtual int get_body(common::ObIAllocator &allocator,
                         common::ObString &model, 
                         common::ObString &prompt,
                         common::ObString &content,
                         common::ObJsonObject *config,
                         common::ObJsonObject *&body) override;
    virtual int parse_output(common::ObIAllocator &allocator,
                             common::ObJsonObject *http_response,
                             common::ObIJsonBase *&result) override;
    virtual int set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config) override;
    static int construct_messages_array(ObIAllocator &allocator, ObString &prompt, ObString &content, ObJsonArray *&messages);
    static int construct_message_obj(ObIAllocator &allocator, ObString &role, ObString &content, ObJsonObject *&message);
  private:
    DISALLOW_COPY_AND_ASSIGN(ObOpenAIComplete);
  };

  class ObOpenAIEmbed : public ObAIFuncIEmbed 
  {
  public:
    ObOpenAIEmbed() {}
    virtual ~ObOpenAIEmbed() {}
    virtual int get_header(common::ObIAllocator &allocator,
                           common::ObString &api_key,
                           common::ObArray<ObString> &headers) override;
    virtual int get_body(common::ObIAllocator &allocator,
                         common::ObString &model, 
                         common::ObArray<ObString> &contents,
                         common::ObJsonObject *config,
                         common::ObJsonObject *&body) override;
    // the same as the above one, but the caller can specify the type of the input content
    int get_body(common::ObIAllocator &allocator,
                 common::ObString &model,
                 common::ObArray<ObString> &contents,
                 common::ObJsonObject *config,
                 common::ObString &input_type,
                 common::ObJsonObject *&body);
    virtual int parse_output(common::ObIAllocator &allocator,
                             common::ObJsonObject *http_response,
                             common::ObIJsonBase *&result) override;
  private:
    DISALLOW_COPY_AND_ASSIGN(ObOpenAIEmbed);
  };
  static int get_header(common::ObIAllocator &allocator,
                        common::ObString &api_key,
                        common::ObArray<ObString> &headers);
private:
  DISALLOW_COPY_AND_ASSIGN(ObOpenAIUtils);
};

class ObOllamaUtils 
{
public:
  class ObOllamaComplete : public ObAIFuncIComplete 
  {
  public:
    ObOllamaComplete() {}
    virtual ~ObOllamaComplete() {}
    virtual int get_header(common::ObIAllocator &allocator,
                           common::ObString &api_key,
                           common::ObArray<ObString> &headers) override;
    virtual int get_body(common::ObIAllocator &allocator,
                         common::ObString &model, 
                         common::ObString &prompt,
                         common::ObString &content,
                         common::ObJsonObject *config,
                         common::ObJsonObject *&body) override;
    virtual int parse_output(common::ObIAllocator &allocator,
                             common::ObJsonObject *http_response,
                             common::ObIJsonBase *&result) override;
    virtual int set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config) override;
  private:
    DISALLOW_COPY_AND_ASSIGN(ObOllamaComplete);
  };

  class ObOllamaEmbed : public ObAIFuncIEmbed 
  {
  public:
    ObOllamaEmbed() {}
    virtual ~ObOllamaEmbed() {}
    virtual int get_header(common::ObIAllocator &allocator,
                           common::ObString &api_key,
                           common::ObArray<ObString> &headers) override;
    virtual int get_body(common::ObIAllocator &allocator,
                         common::ObString &model,
                         common::ObArray<ObString> &contents,
                         common::ObJsonObject *config,
                         common::ObJsonObject *&body) override;
    virtual int parse_output(common::ObIAllocator &allocator,
                             common::ObJsonObject *http_response,
                             common::ObIJsonBase *&result) override;
  private:
    DISALLOW_COPY_AND_ASSIGN(ObOllamaEmbed);
  };
  static int get_header(common::ObIAllocator &allocator, common::ObArray<ObString> &headers);
  DISALLOW_COPY_AND_ASSIGN(ObOllamaUtils);
};

class ObDashscopeUtils 
{
public:
  class ObDashscopeComplete : public ObAIFuncIComplete 
  {
  public:
    ObDashscopeComplete() {}
    virtual ~ObDashscopeComplete() {}
    virtual int get_header(common::ObIAllocator &allocator,
                           common::ObString &api_key,
                           common::ObArray<ObString> &headers) override;
    virtual int get_body(common::ObIAllocator &allocator,
                         common::ObString &model, 
                         common::ObString &prompt,
                         common::ObString &content,
                         common::ObJsonObject *config,
                         common::ObJsonObject *&body) override;
    virtual int parse_output(common::ObIAllocator &allocator,
                             common::ObJsonObject *http_response,
                             common::ObIJsonBase *&result) override;
    virtual int set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config) override;
    static int set_config_result_format(common::ObIAllocator &allocator, common::ObJsonObject *config);
    static int construct_input_obj(ObIAllocator &allocator, ObString &prompt, ObString &content, ObJsonObject *&input_obj);
  private:
    DISALLOW_COPY_AND_ASSIGN(ObDashscopeComplete);
  };
  class ObDashscopeEmbed : public ObAIFuncIEmbed 
  {
  public:
    ObDashscopeEmbed() {}
    virtual ~ObDashscopeEmbed() {}
    virtual int get_header(common::ObIAllocator &allocator,
                           common::ObString &api_key,
                           common::ObArray<ObString> &headers) override;
    virtual int get_body(common::ObIAllocator &allocator,
                         common::ObString &model, 
                         common::ObArray<ObString> &contents,
                         common::ObJsonObject *config,
                         common::ObJsonObject *&body) override;
    virtual int parse_output(common::ObIAllocator &allocator,
                             common::ObJsonObject *http_response,
                             common::ObIJsonBase *&result) override;
  private:
    DISALLOW_COPY_AND_ASSIGN(ObDashscopeEmbed);
  };
  class ObDashscopeRerank : public ObAIFuncIRerank 
  {
  public:
    ObDashscopeRerank() {}
    virtual ~ObDashscopeRerank() {}
    virtual int get_header(common::ObIAllocator &allocator,
                           common::ObString &api_key,
                           common::ObArray<ObString> &headers) override;
    virtual int get_body(common::ObIAllocator &allocator,
                         common::ObString &model,
                         common::ObString &query,
                         common::ObJsonArray *document_array,
                         common::ObJsonObject *config,
                         common::ObJsonObject *&body) override;
    virtual int parse_output(common::ObIAllocator &allocator,
                             common::ObJsonObject *http_response,
                             common::ObIJsonBase *&result) override;
  private:
    static int get_input_obj(common::ObIAllocator &allocator,
                             common::ObString &query,
                             common::ObJsonArray *document_array,
                             common::ObJsonObject *&input_obj);
    DISALLOW_COPY_AND_ASSIGN(ObDashscopeRerank);
  };
  static int get_header(common::ObIAllocator &allocator, common::ObString &api_key, common::ObArray<ObString> &headers);
private:
  DISALLOW_COPY_AND_ASSIGN(ObDashscopeUtils);
};

class ObSiliconflowUtils 
{
public:
  class ObSiliconflowRerank : public ObAIFuncIRerank 
  {
  public:
    ObSiliconflowRerank() {}
    virtual ~ObSiliconflowRerank() {}
    virtual int get_header(common::ObIAllocator &allocator,
                           common::ObString &api_key,
                           common::ObArray<ObString> &headers) override;
    virtual int get_body(common::ObIAllocator &allocator,
                         common::ObString &model,
                         common::ObString &query,
                         common::ObJsonArray *document_array,
                         common::ObJsonObject *config,
                         common::ObJsonObject *&body) override;
    virtual int parse_output(common::ObIAllocator &allocator,
                             common::ObJsonObject *http_response,
                             common::ObIJsonBase *&result) override;
  private:
    DISALLOW_COPY_AND_ASSIGN(ObSiliconflowRerank);
  };
  static int get_header(common::ObIAllocator &allocator,
                        common::ObString &api_key,
                        common::ObArray<ObString> &headers);
private:
  DISALLOW_COPY_AND_ASSIGN(ObSiliconflowUtils);
};
class ObAIFuncJsonUtils 
{
public:
  ObAIFuncJsonUtils() {}
  virtual ~ObAIFuncJsonUtils() {}
  static bool ob_is_json_array_all_str(ObJsonArray* json_array);
  static int transform_array_to_json_array(ObIAllocator &allocator, ObArray<ObString> &contents, ObJsonArray *&array);
  static int get_json_object(ObIAllocator &allocator, ObJsonObject *&obj_node);
  static int get_json_array(ObIAllocator &allocator, ObJsonArray *&array_node);
  static int get_json_string(ObIAllocator &allocator, ObString &str, ObJsonString *&str_node);
  static int get_json_int(ObIAllocator &allocator, int64_t num, ObJsonInt *&int_node);
  static int get_json_boolean(ObIAllocator &allocator, bool value, ObJsonBoolean *&boolean_node);
  static int get_json_object_form_str(ObIAllocator &allocator, const ObString &str, ObJsonObject *&obj_node);
  static int get_json_array_form_str(ObIAllocator &allocator, const ObString &str, ObJsonArray *&array_node);
  static int print_json_to_str(ObIAllocator &allocator, ObIJsonBase *base_node, ObString &str);
  static int compact_json_object(ObIAllocator &allocator, ObJsonObject *obj_node, ObJsonObject *compact_obj);
private:
  DISALLOW_COPY_AND_ASSIGN(ObAIFuncJsonUtils);
};

class ObAIFuncPromptUtils 
{
public:
  ObAIFuncPromptUtils() {}
  virtual ~ObAIFuncPromptUtils() {}
  static int replace_meta_prompt(ObIAllocator &allocator, ObString &meta_prompt, ObString &key, ObString &content, ObString &result);
  DISALLOW_COPY_AND_ASSIGN(ObAIFuncPromptUtils);
};

class ObAIFuncProviderUtils 
{
public:
  ObAIFuncProviderUtils() {}
  virtual ~ObAIFuncProviderUtils() {}
  static constexpr char OPENAI[20] = "OPENAI";
  static constexpr char OLLAMA[20] = "OLLAMA";
  static constexpr char ALIYUN[20] = "ALIYUN-OPENAI";
  static constexpr char DASHSCOPE[20] = "ALIYUN-DASHSCOPE";
  static constexpr char SILICONFLOW[20] = "SILICONFLOW";
  static constexpr char COHERE[20] = "COHERE";
  static constexpr char HUNYUAN[20] = "HUNYUAN-OPENAI";
  static constexpr char DEEPSEEK[20] = "DEEPSEEK";

private:
  DISALLOW_COPY_AND_ASSIGN(ObAIFuncProviderUtils);
};

class ObAIFuncUtils 
{
public:
  ObAIFuncUtils() {}
  virtual ~ObAIFuncUtils() {}
  static bool is_completion_type(const ObAIFuncExprInfo *info) {return info->type_ == share::EndpointType::COMPLETION;}
  static bool is_dense_embedding_type(const ObAIFuncExprInfo *info) {return info->type_ == share::EndpointType::DENSE_EMBEDDING;}
  static bool is_rerank_type(const ObAIFuncExprInfo *info) {return info->type_ == share::EndpointType::RERANK;}
  static int check_info_type_completion(const ObAIFuncExprInfo *info);
  static int check_info_type_dense_embedding(const ObAIFuncExprInfo *info);
  static int check_info_type_rerank(const ObAIFuncExprInfo *info);
  static bool ob_provider_check(const ObString &provider, const char *str_const) { return provider.case_compare(str_const) == 0; }
  static int get_complete_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIComplete *&complete_provider);
  static int get_embed_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIEmbed *&embed_provider);
  static int get_rerank_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIRerank *&rerank_provider);
  static int get_header(ObIAllocator &allocator,
                        const ObAIFuncExprInfo &info,
                        const share::ObAiModelEndpointInfo &endpoint_info,
                        ObArray<ObString> &headers);
  static int get_complete_body(ObIAllocator &allocator, 
                               const ObAIFuncExprInfo &info,
                               const share::ObAiModelEndpointInfo &endpoint_info,
                               ObString &prompt,
                               ObString &content, 
                               ObJsonObject *config,
                               ObJsonObject *&body);
  static int set_json_format_config(ObIAllocator &allocator, const ObString &provider, ObJsonObject *config);
  static int get_embed_body(ObIAllocator &allocator, 
                            const ObAIFuncExprInfo &info,
                            const share::ObAiModelEndpointInfo &endpoint_info,
                            ObArray<ObString> &contents,
                            ObJsonObject *config,
                            ObJsonObject *&body);
  static int get_rerank_body(ObIAllocator &allocator, 
                             const ObAIFuncExprInfo &info,
                             const share::ObAiModelEndpointInfo &endpoint_info,
                             ObString &query,
                             ObJsonArray *document_array,
                             ObJsonObject *config,
                             ObJsonObject *&body);
  static int parse_complete_output(ObIAllocator &allocator, 
                                   const share::ObAiModelEndpointInfo &endpoint_info,
                                   ObJsonObject *http_response,
                                   ObIJsonBase *&result);
  static int parse_embed_output(ObIAllocator &allocator, 
                                const share::ObAiModelEndpointInfo &endpoint_info,
                                ObJsonObject *http_response,
                                ObIJsonBase *&result);
  static int parse_rerank_output(ObIAllocator &allocator, 
                                 const share::ObAiModelEndpointInfo &endpoint_info,
                                 ObJsonObject *http_response,
                                 ObIJsonBase *&result);
  static int set_string_result(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res, ObString &res_str);
  static bool is_provider_support_base64(const ObString &provider);
  static int decode_base64_embedding_array(const ObIJsonBase &embedding_jbase, ObIAllocator &allocator,
                                           const int64_t dimension, float *&vector);
  static int decode_float_embedding_array(const ObIJsonBase &embedding_jbase, ObIAllocator &allocator,
                                          share::ObJsonReaderHelper &json_reader, const int64_t dimension, float *&vector);
  static int get_ai_func_info(ObIAllocator &allocator, const ObString &model_id,
                              share::schema::ObSchemaGetterGuard &guard, ObAIFuncExprInfo *&info);
  static int get_ai_func_info(ObIAllocator &allocator, const ObString &model_id, ObAIFuncExprInfo *&info);
private:
  DISALLOW_COPY_AND_ASSIGN(ObAIFuncUtils);
};


class ObAIFuncModel
{
public:
  ObAIFuncModel(ObIAllocator &allocator, const ObAIFuncExprInfo &info, const share::ObAiModelEndpointInfo &endpoint_info)
  : allocator_(&allocator),
    info_(info),
    endpoint_info_(endpoint_info)
  {}
   virtual ~ObAIFuncModel() {}
   // completion
   int call_completion(ObString &prompt, ObJsonObject *config, ObString &result);
   // completion with an explicit system message, the raw text content of the
   // model response is returned without json escaping.
   int call_completion_with_system(const ObString &system, const ObString &content,
                                   ObJsonObject *config, ObString &result);
   int call_completion_vector(ObArray<ObString> &prompts, ObJsonObject *config, ObArray<ObString> &results);
   // dense_embedding
   int call_dense_embedding(ObString &content, ObJsonObject *config, ObString &result);
   int call_dense_embedding_vector(ObArray<ObString> &contents, ObJsonObject *config, ObArray<ObString> &results);
   int call_dense_embedding_vector_v2(ObArray<ObString> &contents, ObJsonObject *config, ObArray<ObString> &results);
   // rerank
   int call_rerank(ObString &query, ObJsonArray *contents, ObJsonArray *&results);
 private:
   bool is_completion_type() {return info_.type_ == share::EndpointType::COMPLETION;}
   bool is_dense_embedding_type() {return info_.type_ == share::EndpointType::DENSE_EMBEDDING;}
   bool is_rerank_type() {return info_.type_ == share::EndpointType::RERANK;}
   const ObString get_request_model_name();
   ObIAllocator *allocator_;
   const ObAIFuncExprInfo &info_;
   const share::ObAiModelEndpointInfo &endpoint_info_;
   DISALLOW_COPY_AND_ASSIGN(ObAIFuncModel);
};

/**
 * @brief Utils to detect the type of one image binary content and to encode it.
 */
class ObAiFuncImageUtils
{
public:
  enum ObImageType
  {
    IMAGE_TYPE_JPEG = 0,
    IMAGE_TYPE_PNG,
    IMAGE_TYPE_GIF,
    IMAGE_TYPE_WEBP,
    IMAGE_TYPE_BMP,
    IMAGE_TYPE_TIFF,
    IMAGE_TYPE_ICO,
    IMAGE_TYPE_DIB,
    IMAGE_TYPE_ICNS,
    IMAGE_TYPE_SGI,
    IMAGE_TYPE_UNKNOWN
  };
  ObAiFuncImageUtils() {}
  virtual ~ObAiFuncImageUtils() {}
  static int get_type_from_binary(ObIAllocator &allocator, const ObString &binary_image_str, ObImageType &image_type);
  static int get_base64_data_uri_from_binary(ObIAllocator &allocator, const ObString &binary_image_str, ObString &base64_data_uri);
  static const char *get_image_type_str(ObImageType image_type);
private:
  DISALLOW_COPY_AND_ASSIGN(ObAiFuncImageUtils);
};

/**
 * @brief The key of one ai model config, the provider name and the model name
 *        are both compared case insensitively.
 */
class ObAIModelConfigKey
{
public:
  ObAIModelConfigKey() {}
  explicit ObAIModelConfigKey(const ObString &provider, const ObString &model_name)
    : provider_(provider), model_name_(model_name) {}
  virtual ~ObAIModelConfigKey() {}
  uint64_t hash() const
  {
    uint64_t hash_value = 0;
    if (!provider_.empty()) {
      hash_value = murmurhash(provider_.ptr(), static_cast<int32_t>(provider_.length()), 0);
    }
    if (!model_name_.empty()) {
      hash_value = murmurhash(model_name_.ptr(), static_cast<int32_t>(model_name_.length()), hash_value);
    }
    return hash_value;
  }
  bool operator==(const ObAIModelConfigKey &other) const
  {
    return provider_.case_compare(other.provider_) == 0
           && model_name_.case_compare(other.model_name_) == 0;
  }
  ObString provider_;
  ObString model_name_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObAIModelConfigKey);
};

/**
 * @brief The manager of the static ai model configs. It holds the well known
 *        configs of the model services, such as the max batch size and the max
 *        image size of the embedding models.
 */
class ObAIModelConfigInfoManager
{
public:
  ObAIModelConfigInfoManager();
  virtual ~ObAIModelConfigInfoManager();
  static ObAIModelConfigInfoManager &get_instance();
  int init();
  void reset();
  bool is_inited() const { return is_inited_; }
  int get_model_config(const ObString &provider,
                       const ObString &request_model_name,
                       share::ObAIModelConfigItem &item);
  int get_config(const ObString &provider,
                 const ObString &model_name,
                 share::ObAIModelConfigItem &item);
  int set_config(const ObString &provider,
                 const ObString &model_name,
                 const share::ObAIModelConfigItem &item,
                 const bool overwrite = true);
  int64_t size() const { return config_array_.count(); }
private:
  int register_default_model_configs_();
  // the following functions must be called with the lock held
  int init_locked_();
  int set_config_locked_(const ObString &provider,
                         const ObString &model_name,
                         const share::ObAIModelConfigItem &item,
                         const bool overwrite = false);
  struct ModelConfigEntry
  {
    ModelConfigEntry() { provider_[0] = '\0'; model_name_[0] = '\0'; }
    char provider_[64];
    char model_name_[128];
    share::ObAIModelConfigItem item_;
  };
  common::ObSEArray<ModelConfigEntry, 16> config_array_;
  common::ObSpinLock lock_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObAIModelConfigInfoManager);
};

class ObAIFuncPromptObjectUtils
{
public:
  enum class PromptArgType : int8_t
  {
    TEXT = 0,
    IMAGE = 1,
    UNKNOWN = 2
  };
  enum class ImageArgType : int8_t
  {
    URL = 0,
    BINARY = 1
  };
  ObAIFuncPromptObjectUtils() {}
  virtual ~ObAIFuncPromptObjectUtils() {}
  static bool is_valid_prompt_object(ObJsonObject* prompt_object);
  static int replace_all_str_args_in_template(ObIAllocator &allocator, ObJsonObject* prompt_object, ObString& replaced_prompt_str);
  static int construct_prompt_object(ObIAllocator &allocator, ObString &template_str, ObJsonArray *args_array, ObJsonObject *&prompt_object);
  // parse the type of every argument which is referenced by the template
  static int parse_template_arg_types(const ObString &template_str,
                                      const int64_t arg_cnt,
                                      ObIArray<PromptArgType> &arg_types,
                                      bool *has_image = nullptr);
  // build the image argument object which is described by the openai compatible protocol
  static int build_image_arg_object(ObIAllocator &allocator,
                                    const ObString &arg_str,
                                    const ImageArgType image_arg_type,
                                    ObJsonObject *&image_obj);
  // check the size of every binary image argument
  static int validate_binary_image_sizes(ObJsonObject *prompt_object, const int64_t max_image_size);
  static constexpr char prompt_template_key[20] = "template";
  static constexpr char prompt_args_key[20] = "args";
  static constexpr char HTTP_PREFIX[8] = "http://";
  static constexpr char HTTPS_PREFIX[9] = "https://";
private:
  DISALLOW_COPY_AND_ASSIGN(ObAIFuncPromptObjectUtils);
};

} // namespace common
} // namespace oceanbase

#endif /* OB_AI_FUNC_UTILS_H_ */
