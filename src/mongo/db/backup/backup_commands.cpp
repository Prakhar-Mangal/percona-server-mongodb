/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2018-present Percona and/or its affiliates. All rights reserved.

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

#include <boost/filesystem.hpp>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/backup/backupable.h"
#include "mongo/db/commands.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/engine_extension.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {
    extern StorageGlobalParams storageGlobalParams;
}
using namespace mongo;

namespace percona {

class CreateBackupCommand : public ErrmsgCommandDeprecated {
public:
    CreateBackupCommand() : ErrmsgCommandDeprecated("createBackup") {}
    virtual std::string help() const override {
        return "Creates a hot backup, into the given directory, of the files currently in the "
               "storage engine's data directory.\n"
               "{ createBackup: 1, backupDir: <destination directory> }";
    }
    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        return AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                   ResourcePattern::forAnyNormalResource(), ActionType::startBackup)
            ? Status::OK()
            : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }
    bool adminOnly() const override {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    bool errmsgRun(mongo::OperationContext* opCtx,
             const std::string& db,
             const BSONObj& cmdObj,
             std::string& errmsg,
             BSONObjBuilder& result) override;
} createBackupCmd;

bool CreateBackupCommand::errmsgRun(mongo::OperationContext* opCtx,
                              const std::string& db,
                              const BSONObj& cmdObj,
                              std::string& errmsg,
                              BSONObjBuilder& result) {
    BSONElement destPathElem = cmdObj["backupDir"];
    BSONElement tarElem = cmdObj["tar"];
    BSONElement s3Elem = cmdObj["s3"];
    // Command object must specify only one of 'backupDir', 'tar', 's3'
    if ((destPathElem ? 1 : 0) + (tarElem ? 1 : 0) + (s3Elem ? 1 : 0) > 1) {
        errmsg = "Cannot specify more than one of 'backupDir', 'tar' and 's3'";
        return false;
    }

    Status status{Status::OK()};

    if (destPathElem) {
        namespace fs = boost::filesystem;
        const std::string& dest = destPathElem.String();
        fs::path destPath(dest);

        // Validate destination directory.
        try {
            if (!destPath.is_absolute()) {
                errmsg = "Destination path must be absolute";
                return false;
            }

            fs::create_directory(destPath);
        } catch (const fs::filesystem_error& ex) {
            errmsg = ex.what();
            return false;
        }

        // Flush all files first.
        auto se = getGlobalServiceContext()->getStorageEngine();
        se->flushAllFiles(opCtx, true);

        // Do the backup itself.
        status = se->hotBackup(opCtx, dest);

    } else if (tarElem) {
        // Flush all files first.
        auto se = getGlobalServiceContext()->getStorageEngine();
        se->flushAllFiles(opCtx, true);

        // Do the backup itself.
        status = se->hotBackupTar(opCtx, tarElem.String());

    } else if (s3Elem) {
        if (s3Elem.type() != BSONType::Object) {
            errmsg = "'s3' field must be an object";
            return false;
        }

        percona::S3BackupParameters s3params;
        for (auto&& elem : s3Elem.embeddedObject()) {
            if (elem.fieldNameStringData() == "profile"_sd)
                s3params.profile = elem.String();
            else if (elem.fieldNameStringData() == "region"_sd)
                s3params.region = elem.String();
            else if (elem.fieldNameStringData() == "endpoint"_sd)
                s3params.endpoint = elem.String();
            else if (elem.fieldNameStringData() == "scheme"_sd)
                s3params.scheme = elem.String();
            else if (elem.fieldNameStringData() == "useVirtualAddressing"_sd)
                s3params.useVirtualAddressing = elem.Bool();
            else if (elem.fieldNameStringData() == "bucket"_sd)
                s3params.bucket = elem.String();
            else if (elem.fieldNameStringData() == "path"_sd)
                s3params.path = elem.String();
            else {
                errmsg = str::stream()
                    << "s3 subobject contains usupported field or field's name is misspelled: "
                    << elem.fieldName();
                return false;
            }
        }
        if (s3params.bucket.empty()) {
            errmsg = "s3 subobject must provide non-empty 'bucket' field";
            return false;
        }

        // Flush all files first.
        auto se = getGlobalServiceContext()->getStorageEngine();
        se->flushAllFiles(opCtx, true);

        // Do the backup itself.
        status = se->hotBackup(opCtx, s3params);

    } else {
        errmsg = "command object must specify one of 'backupDir', 'tar' or 's3' fields";
        return false;
    }

    if (!status.isOK()) {
        errmsg = status.reason();
        return false;
    }

    return true;
}

}  // end of percona namespace.
