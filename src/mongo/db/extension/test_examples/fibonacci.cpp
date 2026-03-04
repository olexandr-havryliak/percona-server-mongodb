/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in your program, then also delete
 *    it in the license file.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Source stage that emits the first n Fibonacci numbers as documents.
 * Each document has shape: { index: <0-based position>, value: <fibonacci number> }.
 */
class FibonacciExecStage : public sdk::ExecAggStageSource {
public:
    FibonacciExecStage(std::string_view stageName, const BSONObj& arguments)
        : sdk::ExecAggStageSource(stageName), _currentIndex(0) {
        _n = arguments["n"].safeNumberLong();
        if (_n < 0) {
            _n = 0;
        }
    }

    extension::ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                              ::MongoExtensionExecAggStage* execStage) override {
        if (_currentIndex >= _n) {
            return extension::ExtensionGetNextResult::eof();
        }

        long long value = _fibonacci(_currentIndex);
        BSONObj doc = BSON("index" << _currentIndex << "value" << value);
        _currentIndex++;

        return extension::ExtensionGetNextResult::advanced(
            extension::ExtensionBSONObj::makeAsByteBuf(doc));
    }

    void open() override { _currentIndex = 0; }
    void reopen() override { _currentIndex = 0; }
    void close() override {}
    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON("n" << _n);
    }

private:
    static long long _fibonacci(long long k) {
        if (k <= 0)
            return 0;
        if (k == 1)
            return 1;
        long long a = 0, b = 1;
        for (long long i = 2; i <= k; ++i) {
            long long next = a + b;
            a = b;
            b = next;
        }
        return b;
    }

    long long _n;
    long long _currentIndex;
};

class FibonacciLogicalStage : public sdk::TestLogicalStage<FibonacciExecStage> {
public:
    FibonacciLogicalStage(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestLogicalStage<FibonacciExecStage>(stageName, arguments) {}

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<FibonacciLogicalStage>(_name, _arguments);
    }

    boost::optional<extension::sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        extension::sdk::DistributedPlanLogic dpl;
        std::vector<extension::VariantDPLHandle> pipeline;
        pipeline.emplace_back(extension::LogicalAggStageHandle{
            new sdk::ExtensionLogicalAggStageAdapter(clone())});
        dpl.shardsPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        return dpl;
    }
};

class FibonacciAstNode : public sdk::TestAstNode<FibonacciLogicalStage> {
public:
    FibonacciAstNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestAstNode<FibonacciLogicalStage>(stageName, arguments) {}

    BSONObj getProperties() const override {
        extension::MongoExtensionStaticProperties properties;
        properties.setPosition(extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setHostType(
            extension::MongoExtensionHostTypeRequirementEnum::kRunOnceAnyNode);
        properties.setRequiresInputDocSource(false);
        properties.setAllowedInFacet(false);

        BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<FibonacciAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(Fibonacci);

/**
 * $fibonacci accepts an integer n and returns the first n Fibonacci numbers.
 * Syntax: { $fibonacci: { n: <non-negative integer> } }
 * Output documents: { index: <0-based position>, value: <fibonacci number> }
 */
class FibonacciStageDescriptor : public sdk::TestStageDescriptor<"$fibonacci", FibonacciParseNode> {
public:
    void validate(const BSONObj& arguments) const override {
        sdk_uassert(11999010,
                    "Failed to parse $fibonacci: expected object with integer field 'n'",
                    arguments.hasField("n") && arguments.getField("n").isNumber());
        long long n = arguments["n"].safeNumberLong();
        sdk_uassert(11999011, "$fibonacci: 'n' must be non-negative", n >= 0);
        sdk_uassert(11999012, "$fibonacci: 'n' must be at most 1000", n <= 1000);
    }
};

DEFAULT_EXTENSION(Fibonacci)
REGISTER_EXTENSION(FibonacciExtension)
DEFINE_GET_EXTENSION()
