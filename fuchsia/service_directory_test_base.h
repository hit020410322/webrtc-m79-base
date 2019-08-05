// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_FUCHSIA_SERVICE_DIRECTORY_TEST_BASE_H_
#define BASE_FUCHSIA_SERVICE_DIRECTORY_TEST_BASE_H_

#include <zircon/types.h>
#include <memory>

#include "base/fuchsia/scoped_service_binding.h"
#include "base/fuchsia/service_directory_client.h"
#include "base/fuchsia/test_interface_impl.h"
#include "base/fuchsia/testfidl/cpp/fidl.h"
#include "base/run_loop.h"
#include "base/test/scoped_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace base {
namespace fuchsia {

class ServiceDirectoryTestBase : public testing::Test {
 public:
  ServiceDirectoryTestBase();
  ~ServiceDirectoryTestBase() override;

  void VerifyTestInterface(fidl::InterfacePtr<testfidl::TestInterface>* stub,
                           zx_status_t expected_error);

 protected:
  const RunLoop::ScopedRunTimeoutForTest run_timeout_;

  base::test::ScopedTaskEnvironment scoped_task_environment_{
      base::test::ScopedTaskEnvironment::ThreadingMode::MAIN_THREAD_ONLY,
      base::test::ScopedTaskEnvironment::MainThreadType::IO};

  std::unique_ptr<ServiceDirectory> service_directory_;
  TestInterfaceImpl test_service_;
  std::unique_ptr<ScopedServiceBinding<testfidl::TestInterface>>
      service_binding_;

  std::unique_ptr<ServiceDirectoryClient> public_service_directory_client_;
  std::unique_ptr<ServiceDirectoryClient> debug_service_directory_client_;
  std::unique_ptr<ServiceDirectoryClient>
      legacy_public_service_directory_client_;
  std::unique_ptr<ServiceDirectoryClient> root_service_directory_client_;

  DISALLOW_COPY_AND_ASSIGN(ServiceDirectoryTestBase);
};

}  // namespace fuchsia
}  // namespace base

#endif  // BASE_FUCHSIA_SERVICE_DIRECTORY_TEST_BASE_H_
