// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "low_precision/convolution_backprop_data.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <cassert>

#include <ngraph/pattern/op/wrap_type.hpp>
#include <ngraph/pattern/op/or.hpp>
#include "low_precision/network_helper.hpp"
#include <transformations/rt_info/disable_constant_folding.hpp>
#include "itt.hpp"

namespace ngraph {
namespace pass {
namespace low_precision {

ConvolutionBackpropDataTransformation::ConvolutionBackpropDataTransformation(const Params& params) : WeightableLayerTransformation(params) {
    MATCHER_SCOPE(ConvolutionBackpropDataTransformation);
    auto matcher = std::make_shared<pattern::op::Or>(OutputVector{
        pattern::wrap_type<ov::opset1::ConvolutionBackpropData>({
            pattern::wrap_type<ov::opset1::Multiply>(),
            pattern::wrap_type<ov::opset1::Multiply>()
        }),
        ngraph::pattern::wrap_type<ov::opset1::ConvolutionBackpropData>({
            pattern::wrap_type<ov::opset1::Multiply>(),
            pattern::wrap_type<ov::opset1::FakeQuantize>()
        }),
        ngraph::pattern::wrap_type<ov::opset1::ConvolutionBackpropData>({
            pattern::wrap_type<ov::opset1::Multiply>(),
            pattern::wrap_type<ov::opset1::Multiply>(),
            pattern::wrap_type<ov::opset1::Constant>()
        }),
        ngraph::pattern::wrap_type<ov::opset1::ConvolutionBackpropData>({
            pattern::wrap_type<ov::opset1::Multiply>(),
            pattern::wrap_type<ov::opset1::FakeQuantize>(),
            pattern::wrap_type<ov::opset1::Constant>()
        }),
    });

    ov::graph_rewrite_callback callback = [this](pattern::Matcher& m) {
        auto op = m.get_match_root();
        if (transformation_callback(op)) {
            return false;
        }
        return transform(*context, m);
    };

    auto m = std::make_shared<ngraph::pattern::Matcher>(matcher, matcher_name);
    this->register_matcher(m, callback);
}

bool ConvolutionBackpropDataTransformation::isQuantized(const std::shared_ptr<const Node>& layer,
    const std::vector<ngraph::element::Type>& defaultPrecisions) const {
    return ConvolutionBackpropDataTransformation::isQuantizedStatic(layer, defaultPrecisions);
}

bool ConvolutionBackpropDataTransformation::isQuantizedStatic(const std::shared_ptr<const Node>& layer,
    const std::vector<ngraph::element::Type>& defaultPrecisions) {
    return WeightableLayerTransformation::isQuantizedStatic(layer, false, defaultPrecisions);
}

size_t ConvolutionBackpropDataTransformation::getInputChannels(const std::shared_ptr<ngraph::Node> conv) const {
    const auto channels = conv->get_input_partial_shape(1)[0];
    assert(channels.is_static());
    return channels.get_length();
}

bool ConvolutionBackpropDataTransformation::transform(TransformationContext &context, ngraph::pattern::Matcher &m) {
    auto convolutionBackpropData = m.get_match_root();

    if (!canBeTransformed(context, convolutionBackpropData)) {
        auto weightsInput = convolutionBackpropData->get_input_node_shared_ptr(1);
        std::shared_ptr<ov::opset1::Reshape> reshapeFromWeights = ov::as_type_ptr<ov::opset1::Reshape>(weightsInput);
        FakeQuantizeDequantization dequantization = reshapeFromWeights == nullptr ?
                         NetworkHelper::getDequantization(convolutionBackpropData, defaultPrecisions, 1ul) :
                         NetworkHelper::getDequantization(reshapeFromWeights, defaultPrecisions);
        if (dequantization.empty()) {
            const auto fqOnWeights = getFakeQuantizeOnWeights(convolutionBackpropData);
            auto constantShape = fqOnWeights->input(1).get_partial_shape();
            if (constantShape.is_dynamic() || constantShape.rank().is_dynamic()) {
                return false;
            }

            auto resultConstant = NetworkHelper::fold_fake_quantize(fqOnWeights, false);
            if (reshapeFromWeights != nullptr) {
                resultConstant = fold_reshape<ov::opset1::Reshape>(
                        resultConstant,
                        reshapeFromWeights->input_value(1),
                        false);
            }
            if (ov::is_type<ov::opset1::Constant>(resultConstant)) {
                replace_node(weightsInput, resultConstant);
            }
        } else {
            NetworkHelper::foldDequantization(dequantization.multiply, 0, defaultPrecisions, true);
        }
        return true;
    }

    convolutionBackpropData = NetworkHelper::separateInStandaloneBranch(convolutionBackpropData, defaultPrecisions);
    FakeQuantizeDequantization dequantization = NetworkHelper::getDequantization(convolutionBackpropData, defaultPrecisions);
    std::shared_ptr<Node> newMultiplyAfter;
    {
        if (dequantization.subtract != nullptr) {
            NetworkHelper::optimizeSubtract(dequantization.subtract);
        }

        std::shared_ptr<Node> newMultiplyAfterConst = std::make_shared<ov::opset1::Constant>(
            dequantization.multiplyConstant->get_element_type(),
            Shape{ 1 },
            dequantization.multiplyConstant->cast_vector<float>()[0]);
        auto inputs = convolutionBackpropData->input_values();
        inputs[0] = dequantization.multiply->input_value(0);
        const auto copyNode = convolutionBackpropData->clone_with_new_inputs(inputs);

        const auto relaxedConvolutionBackpropData = std::make_shared<ov::op::TypeRelaxed<ov::opset1::ConvolutionBackpropData>>(
            *ov::as_type_ptr<ov::opset1::ConvolutionBackpropData>(copyNode),
            std::vector<element::Type>{deqPrecision, deqPrecision},
            std::vector<element::Type>{deqPrecision});

        newMultiplyAfter = std::make_shared<ov::op::TypeRelaxed<ov::opset1::Multiply>>(
            std::vector<element::Type>{ deqPrecision, deqPrecision },
            std::vector<element::Type>{ dequantization.multiply->get_output_element_type(0) },
            ov::op::TemporaryReplaceOutputType(relaxedConvolutionBackpropData, deqPrecision).get(),
            ov::op::TemporaryReplaceOutputType(newMultiplyAfterConst, deqPrecision).get());
        NetworkHelper::insertDequantizationAfter(convolutionBackpropData, newMultiplyAfter, relaxedConvolutionBackpropData);

        convolutionBackpropData = newMultiplyAfter->get_input_node_shared_ptr(0);
        inputs[0] = convolutionBackpropData->get_input_node_ptr(0)->input_value(0);
        if (ov::is_type<ov::opset1::Convert>(convolutionBackpropData->get_input_node_ptr(0))) {
            auto newConvolution = convolutionBackpropData->clone_with_new_inputs(inputs);
            replace_node(convolutionBackpropData, newConvolution);
            convolutionBackpropData = newConvolution;
        }
    }

    {
        decomposeFakeQuantizeForWeightsPath(convolutionBackpropData, 1ul);
        dequantization = NetworkHelper::getDequantization(convolutionBackpropData, defaultPrecisions, 1ul);

        if (const auto fq = ov::as_type_ptr<ov::opset1::FakeQuantize>(dequantization.data.get_node_shared_ptr())) {
            const auto newFQ = NetworkHelper::fold_fake_quantize(fq, true);
            NetworkHelper::copyInfo(fq, newFQ);
            replace_node(fq, newFQ);
        }

        const auto multiplyFromWeights = convolutionBackpropData->get_input_node_shared_ptr(1);
        auto subtractFromWeights = ov::as_type_ptr<ov::opset1::Subtract>(multiplyFromWeights->get_input_node_shared_ptr(0));

        {
            const auto newScalePShape = multiplyFromWeights->get_input_partial_shape(1);
            assert(newScalePShape.is_static());
            Shape newScaleShape = newScalePShape.to_shape();

            auto inputs = convolutionBackpropData->input_values();
            inputs[1] = multiplyFromWeights->input_value(0);

            const auto newconvolutionBackpropData = convolutionBackpropData->copy_with_new_inputs(inputs);
            newMultiplyAfter = std::make_shared<ov::opset1::Multiply>(
                newconvolutionBackpropData,
                foldConvert(
                    fold_reshape<ov::opset1::Reshape>(
                        multiplyFromWeights->input_value(1),
                        std::make_shared<ov::opset1::Constant>(element::u64, Shape{ newScaleShape.size() }, newScaleShape),
                        false),
                    convolutionBackpropData->get_output_element_type(0)));
            NetworkHelper::insertDequantizationAfter(convolutionBackpropData, newMultiplyAfter, newconvolutionBackpropData);
            convolutionBackpropData = newMultiplyAfter->get_input_node_shared_ptr(0);
        }

        if (subtractFromWeights != nullptr) {
            // optimize zero point on weights
            auto optimizedSubtract = NetworkHelper::optimizeSubtract(subtractFromWeights);
            if (optimizedSubtract == nullptr) {
                subtractFromWeights = nullptr;
            } else {
                subtractFromWeights = ov::as_type_ptr<ov::opset1::Subtract>(optimizedSubtract);

                const auto weightsPShape = subtractFromWeights->get_input_partial_shape(0);
                assert(weightsPShape.is_static());

                const size_t weightsRankValue = weightsPShape.rank().get_length();
                Shape zeroPointShape(weightsRankValue, 1ul);
                zeroPointShape[1] = static_cast<size_t>(weightsPShape[1].get_length());

                auto zeroPointConstant = fold<ov::opset1::Broadcast>(
                        subtractFromWeights->input_value(1),
                        std::make_shared<ov::opset1::Constant>(element::i32, Shape{zeroPointShape.size()}, zeroPointShape));
                replace_node(subtractFromWeights->get_input_node_shared_ptr(1), zeroPointConstant);
            }
        }

        std::shared_ptr<ov::opset1::Convert> convertFromWeights =
                ov::as_type_ptr<ov::opset1::Convert>(
                    subtractFromWeights == nullptr ?
                        multiplyFromWeights->get_input_node_shared_ptr(0) :
                        subtractFromWeights->get_input_node_shared_ptr(0));
        if (convertFromWeights != nullptr) {
            auto inputs = convolutionBackpropData->input_values();
            inputs[1] = convolutionBackpropData->get_input_node_ptr(1)->input_value(0);
            // remove Convert on weights
            auto newConvolution = convolutionBackpropData->clone_with_new_inputs(inputs);
            replace_node(convolutionBackpropData, newConvolution);
            convolutionBackpropData = newConvolution;
        }
    }

    const auto finalDequantization = NetworkHelper::optimizeMultipliesAfter(newMultiplyAfter);
    ov::copy_runtime_info({ convolutionBackpropData, finalDequantization }, finalDequantization);
    updateOutput(context, finalDequantization, convolutionBackpropData);

    auto onWeights = convolutionBackpropData->get_input_node_shared_ptr(1);
    if (ov::is_type<ov::opset1::Reshape>(onWeights)) {
        onWeights = onWeights->get_input_node_shared_ptr(0);
    }

    if (ov::is_type<ov::opset1::Subtract>(onWeights)) {
        ov::disable_constant_folding(onWeights);
    }

    return true;
}

bool ConvolutionBackpropDataTransformation::canBeTransformed(const TransformationContext& context, std::shared_ptr<Node> op) const {
    return canConvolutionBeTransformed(context, op, defaultPrecisions);
}

} // namespace low_precision
} // namespace pass
} // namespace ngraph
