// Copyright 2026 The ODML Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/engine/litert_lm_lib.h"

#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/status/status.h"
#include "runtime/engine/engine_settings.h"
#include "runtime/executor/llm_executor_settings.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {
using ::testing::status::StatusIs;
TEST(LiteRtLmLibTest, NpuTokenIdScrubbingIsOptIn) {
  const auto model_path = std::filesystem::path(::testing::SrcDir()) /
                          "litert_lm/runtime/testdata/test_lm.litertlm";
  LiteRtLmSettings settings;
  settings.model_path = model_path.string();
  settings.backend = "npu";
  for (bool enabled : {false, true}) {
    settings.enable_npu_token_id_scrubbing = enabled;
    ASSERT_OK_AND_ASSIGN(auto engine_settings, CreateEngineSettings(settings));
    ASSERT_OK_AND_ASSIGN(auto config,
                         engine_settings.GetMainExecutorSettings()
                             .GetBackendConfig<NpuConfig>());
    EXPECT_EQ(config.enable_token_id_scrubbing, enabled);
  }
  settings.backend = "cpu";
  EXPECT_THAT(CreateEngineSettings(settings),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace
}  // namespace litert::lm
