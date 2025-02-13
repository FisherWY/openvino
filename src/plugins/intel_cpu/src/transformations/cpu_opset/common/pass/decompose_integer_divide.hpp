// Copyright (C) 2020-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <ngraph/pattern/op/wrap_type.hpp>
#include <ngraph/pass/graph_rewrite.hpp>

namespace ov {
namespace intel_cpu {

class DecomposeIntegerDivide: public ngraph::pass::MatcherPass {
public:
    OPENVINO_RTTI("DecomposeIntegerDivide", "0");
    DecomposeIntegerDivide();
};

}   // namespace intel_cpu
}   // namespace ov
