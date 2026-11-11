# Unit tests of the ai functions

This directory holds the unit tests of the ai functions of seekdb:

| file | content |
| ---- | ------- |
| `ob_expr_ai_prompt_test.cpp` | the tests of the prompt object utils (`ObAIFuncPromptObjectUtils`) and of the json utils (`ObAIFuncJsonUtils`) |
| `test_ob_openai_utils.cpp` | the tests of the openai compatible protocol utils (`ObOpenAIUtils`) and of the ai model config manager (`ObAIModelConfigInfoManager`) |
| `ob_expr_test_utils.h` | the common helpers of the expression tests |

## How to build and run them

The repository does not ship a googletest dependency yet, so the tests are
delivered as the sources only. To run them, add a googletest dependency and a
`cc_test` target to the bazel build graph, the pattern of the other unit test
directories of the repository can be used, for example
`unittest/storage/memtable/BUILD.bazel` of the `master` branch:

```python
cc_test(
    name = "ob_expr_ai_prompt_test",
    srcs = ["ob_expr_ai_prompt_test.cpp"],
    copts = ["-std=c++20"],
    features = ["-static_cpp_runtime", "-static_link_cpp_runtimes"],
    linkstatic = True,
    deps = [
        "//src/observer:liboceanbase",
        "@com_google_googletest//:gtest_main",
    ],
    target_compatible_with = ["@platforms//os:linux"],
)
```

The `test_ob_openai_utils` target is built in the same way, the entry point of
the tests is the `main` function of googletest, so `gtest_main` is used
instead of `gtest`.

The tests only depend on the public interfaces of
`src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h`, so they can also be
compiled and run standalone against the library of the observer.
