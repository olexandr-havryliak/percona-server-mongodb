/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2023-present Percona and/or its affiliates. All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the Server Side Public License, version 1,
    as published by MongoDB, Inc.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Server Side Public License for more details.

    You should have received a copy of the Server Side Public License
    along with this program. If not, see
    <http://www.mongodb.com/licensing/server-side-public-license>.

    As a special exception, the copyright holders give permission to link the
    code of portions of this program with the OpenSSL library under certain
    conditions as described in each individual source file and distribute
    linked combinations including the program with the OpenSSL library. You
    must comply with the Server Side Public License in all respects for
    all of the code used other than as permitted herein. If you modify file(s)
    with this exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do so,
    delete this exception statement from your version. If you delete this
    exception statement from all source files in the program, then also delete
    it in the license file.
======= */

#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/sasl_mechanism_policies.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/user.h"

namespace mongo {

class BSONObjBuilder;
class OperationContext;

namespace awsIam {

class ServerMechanism final : public MakeServerMechanism<AWSIAMPolicy> {
public:
    ServerMechanism(const ServerMechanism&) = delete;
    ServerMechanism& operator=(const ServerMechanism&) = delete;

    ServerMechanism(ServerMechanism&&) = delete;
    ServerMechanism& operator=(ServerMechanism&&) = delete;

    explicit ServerMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<AWSIAMPolicy>(std::move(authenticationDatabase)) {}

    ~ServerMechanism() final = default;

private:
    std::uint32_t _step{0};

    std::vector<char> _serverNonce;
    std::int32_t _gs2_cb_flag = 0;
    std::string _userId;

    void appendExtraInfo(BSONObjBuilder* bob) const final;
    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       StringData input) final;
    StatusWith<std::tuple<bool, std::string>> _firstStep(StringData inputData);
    StatusWith<std::tuple<bool, std::string>> _secondStep(StringData inputData);
    void _parseStsResponse(StringData body);
};

class ServerFactory final : public MakeServerFactory<ServerMechanism> {
public:
    using MakeServerFactory<ServerMechanism>::MakeServerFactory;
    static constexpr bool isInternal = false;

    bool canMakeMechanismForUser(const User* user) const final {
        auto credentials = user->getCredentials();
        return credentials.isExternal &&
            (credentials.scram<SHA1Block>().isValid() ||
             credentials.scram<SHA256Block>().isValid());
    }
};

}  // namespace awsIam
}  // namespace mongo
