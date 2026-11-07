#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <stdexcept>

inline bool isDiabloSaveFile(const QString &name) {
    const QString ext = QFileInfo(name).suffix().toLower();
    return ext == "d2s" || ext == "d2x" || ext == "key" || ext == "map" ||
        ext == "sss" || ext == "d2i" ||
        (ext.size() == 3 && ext.startsWith("ma") && ext[2].isDigit());
}

inline void preserveDiabloSaves(const QStringList &sources, const QString &destination) {
    if (!QDir().mkpath(destination)) throw std::runtime_error("cannot create persistent save directory");
    for (const QString &source : sources) {
        for (const QFileInfo &file : QDir(source).entryInfoList(QDir::Files)) {
            if (!isDiabloSaveFile(file.fileName())) continue;
            QFile input(file.absoluteFilePath());
            if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read an existing save; shadow cleanup cancelled");
            const QByteArray data = input.readAll();
            if (input.error() != QFile::NoError) throw std::runtime_error("cannot read an existing save completely");
            QString target = QDir(destination).filePath(file.fileName());
            if (QFileInfo::exists(target)) {
                QFile existing(target);
                if (!existing.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read persistent save");
                if (existing.readAll() == data) continue;
                const QString digest = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
                const QString backup = QDir(destination).filePath("migration-backups/" + digest);
                if (!QDir().mkpath(backup)) throw std::runtime_error("cannot create save migration backup");
                target = QDir(backup).filePath(file.fileName());
                if (QFileInfo::exists(target)) continue;
            }
            QSaveFile output(target);
            if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit())
                throw std::runtime_error("cannot preserve existing save; shadow cleanup cancelled");
            QFile copied(target);
            if (!copied.open(QIODevice::ReadOnly) || copied.readAll() != data)
                throw std::runtime_error("save migration verification failed; shadow cleanup cancelled");
        }
    }
}
