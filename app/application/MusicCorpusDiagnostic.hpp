#pragma once

#include <QJsonObject>
#include <QString>

struct Jam2MusicCorpusOptions {
    bool includeAudio = true;
    bool drumOnlyAudio = false;
    bool matchedComplexitySeeds = false;
    int samplesPerCell = 4;
    // Zero enumerates native forms. A positive value emits one custom form of
    // this length per profile, which is used to author the groove library.
    int fixedBars = 0;
    QString styleId;
    QString profileId;
};

QJsonObject jam2WriteFullFormMusicCorpus(
    const QString& artifactRoot,
    const QString& seedNamespace,
    const Jam2MusicCorpusOptions& options = {});
