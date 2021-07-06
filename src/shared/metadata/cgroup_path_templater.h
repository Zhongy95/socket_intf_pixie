/*
 * Copyright 2018- The Pixie Authors.
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
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/common/base/base.h"
#include "src/common/system/system.h"
#include "src/shared/metadata/k8s_objects.h"

namespace px {
namespace md {

// To find the processes that belong to a pod, we need to consult /sys/fs/cgroup paths.
// Unfortunately, there many are different cgroup naming formats used by k8s under sysfs.
//
// Some examples include:
//  /sys/fs/cgroup/cpu,cpuacct/kubepods/pod8dbc5577-d0e2-4706-8787-57d52c03ddf2/14011c7d92a9e513dfd69211da0413dbf319a5e45a02b354ba6e98e10272542d/cgroup.procs
//  /sys/fs/cgroup/cpu,cpuacct/kubepods.slice/kubepods-pod8dbc5577_d0e2_4706_8787_57d52c03ddf2.slice/docker-14011c7d92a9e513dfd69211da0413dbf319a5e45a02b354ba6e98e10272542d.scope/cgroup.procs
//  /sys/fs/cgroup/cpu,cpuacct/kubepods.slice/kubepods-pod8dbc5577_d0e2_4706_8787_57d52c03ddf2.slice/crio-14011c7d92a9e513dfd69211da0413dbf319a5e45a02b354ba6e98e10272542d.scope/cgroup.procs
//  /sys/fs/cgroup/cpu,cpuacct/system.slice/containerd.service/kubepods-besteffort-pod1544eb37_e4f7_49eb_8cc4_3d01c41be77b.slice:cri-containerd:8618d3540ce713dd59ed0549719643a71dd482c40c21685773e7ac1291b004f5/cgroup.procs
//
// These variations (and future ones) makes it hard to know where to look for cgroup.procs files.
//
// This file provides functions to help manage this problem.
// It consists of:
//  * AutoDiscoverCGroupTemplate() which finds the cgroup.procs files for the current pod (assuming
//    we are deployed on K8s). It then uses the file as an example from which it creates a template.
//
//  * CGroupTemplater which takes the path template above and provides an interface to generate
//    paths for other pods.

/**
 * A templated CGroup path, which encodes the format of CGroup paths.
 */
struct CGroupTemplateSpec {
  /**
   * The templated path, where:
   *   pod id -> $0
   *   container id -> $1
   *   qos -> $2
   *
   * Example: /sys/fs/cgroup/cpu,cpuacct/kubepods/$2/$0/$1/cgroup.procs
   * Note that the "guaranteed" QoS is usually implicit, a fact that
   * is handled by the CGroupTemplater below.
   */
  std::string templated_path;

  /**
   * Whether the pod ID in the path uses dashes or underscores.
   */
  std::optional<char> pod_id_separators = '_';

  /**
   * Whether we expect to see kubepods/qos or kubepods-qos.
   */
  char qos_separator = '/';
};

/**
 * Searches /sys/fs to find a cgroup path to use.
 * Typically, we rely on /sys/fs/cgroup/cpu,cpuacct, but we have run into systems
 * that don't have that path, which makes this function necessary.
 */
StatusOr<std::string> CGroupBasePath(std::string_view sysfs_path);

/**
 * Finds the cgroup.procs file for the current process, assuming it is in a pod.
 */
StatusOr<std::string> FindSelfCGroupProcs(std::string_view base_path);

/**
 * Given a path to a sample cgroup.procs file for a K8s pod,
 * this function produces a templated spec from which paths for other pods can be generated.
 */
StatusOr<CGroupTemplateSpec> CreateCGroupTemplateSpecFromPath(std::string_view example_path);

StatusOr<CGroupTemplateSpec> AutoDiscoverCGroupTemplate(std::string_view sysfs_path);

class CGroupTemplater {
 public:
  explicit CGroupTemplater(CGroupTemplateSpec cgroup_spec) : spec_(std::move(cgroup_spec)) {}

  std::string Evaluate(PodQOSClass qos_class, std::string_view pod_id,
                       std::string_view container_id);

 private:
  CGroupTemplateSpec spec_;
};

}  // namespace md
}  // namespace px
