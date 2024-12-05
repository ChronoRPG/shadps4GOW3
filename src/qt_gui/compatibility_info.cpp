// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QProgressDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <iostream>
#include "common/path_util.h"
#include "compatibility_info.h"

CompatibilityInfoClass::CompatibilityInfoClass()
    : m_network_manager(new QNetworkAccessManager(this)) {
    QStringList file_paths;
    std::filesystem::path compatibility_file_path =
        Common::FS::GetUserPath(Common::FS::PathType::MetaDataDir) / "compatibility_data.json";
    Common::FS::PathToQString(m_compatibility_filename, compatibility_file_path);
};
CompatibilityInfoClass::~CompatibilityInfoClass() = default;

void CompatibilityInfoClass::UpdateCompatibilityDatabase(QWidget* parent) {
    QFileInfo check_file(m_compatibility_filename);
    const auto modified_delta = check_file.lastModified() - QDateTime::currentDateTime();
    if (check_file.exists() && check_file.isFile() && std::chrono::duration_cast<std::chrono::minutes>(modified_delta).count() < 60) {
        if (LoadCompatibilityFile())
            return;
        QMessageBox::critical(parent, tr("Error"),
                              tr("Failure in reading compatibility_data.json."));
    }

    QNetworkReply* reply = FetchPage(1);
    WaitForReply(reply);

    QProgressDialog dialog(tr("Fetching compatibility data, please wait"), tr("Cancel"), 0, 0,
                           parent);
    dialog.setWindowTitle(tr("Loading..."));

    int remaining_pages = 0;
    if (reply->hasRawHeader("link")) {
        QRegularExpression last_page_re("(\\d+)(?=>; rel=\"last\")");
        QRegularExpressionMatch last_page_match =
            last_page_re.match(QString(reply->rawHeader("link")));
        if (last_page_match.hasMatch()) {
            remaining_pages = last_page_match.captured(0).toInt() - 1;
        }
    }

    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        QMessageBox::critical(parent, tr("Error"),
                              tr("Unable to update compatibility data! Using old compatibility data..."));
        //TODO: Try loading compatibility_file.json again 
        LoadCompatibilityFile();
        return;
    }

    ExtractCompatibilityInfo(reply->readAll());

    QVector<QNetworkReply*> replies(remaining_pages);
    QFutureWatcher<void> future_watcher;

    for (int i = 0; i < remaining_pages; i++) {
        replies[i] = FetchPage(i + 2);
    }

    future_watcher.setFuture(QtConcurrent::map(replies, WaitForReply));
    connect(&future_watcher, &QFutureWatcher<QByteArray>::finished, [&]() {
        for (int i = 0; i < remaining_pages; i++) {
            if (replies[i]->error() == QNetworkReply::NoError) {
                ExtractCompatibilityInfo(replies[i]->readAll());
            }
            replies[i]->deleteLater();
        }

        QFile compatibility_file(m_compatibility_filename);

        if (!compatibility_file.open(QIODevice::WriteOnly | QIODevice::Truncate |
                                     QIODevice::Text)) {
            QMessageBox::critical(parent, tr("Error"),
                                  tr("Unable to open compatibility.json for writing."));
            return;
        }

        QJsonDocument json_doc;
        json_doc.setObject(m_compatibility_database);
        compatibility_file.write(json_doc.toJson());
        compatibility_file.close();

        dialog.reset();
    });
    connect(&dialog, &QProgressDialog::canceled, &future_watcher,
            &QFutureWatcher<void>::cancel);
    dialog.setRange(0, remaining_pages);
    connect(&future_watcher, &QFutureWatcher<void>::progressValueChanged, &dialog,
            &QProgressDialog::setValue);
    dialog.exec();
}

QNetworkReply* CompatibilityInfoClass::FetchPage(int page_num) {
    QUrl url = QUrl("https://api.github.com/repos/shadps4-emu/shadps4-game-compatibility/issues");
    QUrlQuery query;
    query.addQueryItem("per_page", QString("100"));
    query.addQueryItem(
        "tags", QString("status-ingame status-playable status-nothing status-boots status-menus"));
    query.addQueryItem("page", QString::number(page_num));
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply* reply = m_network_manager->get(request);

    return reply;
}

void CompatibilityInfoClass::WaitForReply(QNetworkReply* reply) {
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    return;
};

CompatibilityStatus CompatibilityInfoClass::GetCompatibilityStatus(const std::string& serial) {
    QString title_id = QString::fromStdString(serial);
    if (m_compatibility_database.contains(title_id)) {
        {
            for (int os_int = 0; os_int != OSType::Last; os_int++) {
                QString os_string = OSTypeToString.at(static_cast<OSType>(os_int));
                QJsonObject compatibility_obj = m_compatibility_database[title_id].toObject();
                if (compatibility_obj.contains(os_string)) {
                    return LabelToCompatStatus.at(
                        compatibility_obj[os_string].toString());
                }
            }
        }
    }
    return CompatibilityStatus::Unknown;
}

bool CompatibilityInfoClass::LoadCompatibilityFile() {
    QFile compatibility_file(m_compatibility_filename);
    if (!compatibility_file.open(QIODevice::ReadOnly)) {
        compatibility_file.close();
        return false;
    }
    QByteArray json_data = compatibility_file.readAll();
    compatibility_file.close();

    QJsonDocument json_doc = QJsonDocument::fromJson(json_data);
    if (json_doc.isEmpty() || json_doc.isNull()) {
        return false;
    }

    m_compatibility_database = json_doc.object();
    return true;
}


void CompatibilityInfoClass::ExtractCompatibilityInfo(QByteArray response) {
    QJsonDocument json_doc(QJsonDocument::fromJson(response));

    if (json_doc.isNull()) {
        return;
    }

    QJsonArray json_arr;

    json_arr = json_doc.array();

    for (const auto& issue_ref : std::as_const(json_arr)) {
        QJsonObject issue_obj = issue_ref.toObject();
        QString title_id;
        QRegularExpression title_id_regex("CUSA[0-9]{5}");
        QRegularExpressionMatch title_id_match =
            title_id_regex.match(issue_obj["title"].toString());
        QString current_os = "os-unknown";
        QString compatibility_status = "status-unknown";
        if (issue_obj.contains("labels") && title_id_match.hasMatch()) {
            title_id = title_id_match.captured(0);
            const QJsonArray& label_array = issue_obj["labels"].toArray();
            for (const auto& elem : label_array) {
                QString label = elem.toObject()["name"].toString();
                if (LabelToOSType.contains(label)) {
                    current_os = label;
                    continue;
                }
                if (LabelToCompatStatus.contains(label)) {
                    compatibility_status = label;
                    continue;
                }
            }

            QJsonValueRef compatibility_object_ref = m_compatibility_database[title_id];

            if (compatibility_object_ref.isNull()) {
                compatibility_object_ref = QJsonObject({{current_os, compatibility_status}});
            } else {
                compatibility_object_ref.toObject()[current_os] = compatibility_status;
            }
        }
    }

    return;
}
