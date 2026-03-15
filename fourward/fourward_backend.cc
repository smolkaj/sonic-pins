// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fourward/fourward_backend.h"

#include "absl/status/statusor.h"
#include "gutil/gutil/proto.h"
#include "p4_infra/p4_pdpi/ir.pb.h"

namespace fourward {

absl::StatusOr<pdpi::IrEntities> FourwardBackend::GetEntitiesToPuntAllPackets(
    const pdpi::IrP4Info& switch_p4info) const {
  // SAI P4 ACL entry that matches all packets and punts them to the controller.
  // This is installed on the control switch so it punts all received packets,
  // allowing DVaaS to collect forwarded packets from the SUT.
  //
  // TODO(4ward): Make this configurable rather than hardcoding SAI P4.
  constexpr char kPuntAllEntities[] = R"pb(
    entities {
      table_entry {
        table_name: "acl_ingress_table"
        priority: 1
        action {
          name: "acl_trap"
          params {
            name: "qos_queue"
            value { str: "0x7" }
          }
        }
      }
    }
  )pb";

  pdpi::IrEntities entities;
  RETURN_IF_ERROR(gutil::ReadProtoFromString(kPuntAllEntities, &entities));
  return entities;
}

}  // namespace fourward
