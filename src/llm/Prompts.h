#pragma once
#include <QHash>
#include <QString>

namespace gromarch {
namespace prompts {

// Max transcript characters sent to the model; longer transcripts keep the tail.
inline constexpr int kMaxTranscriptChars = 48000;
// Transcript prefix used for the title suggestion job.
inline constexpr int kTitleTranscriptChars = 4000;

// The Granola trick: the cues the user typed are the signal for what mattered;
// the transcript is only there to supply the specifics.
inline QString systemBase() {
    return QStringLiteral(
        "You are a meeting-notes editor. The user typed rough cues during the meeting; "
        "these indicate what MATTERED to them. Merge the cues with the transcript into "
        "polished Markdown notes. The cues drive emphasis and structure; the transcript "
        "fills in specifics (names, numbers, decisions, action items). Never invent content "
        "not supported by the transcript. Output only the note.");
}

// Per-template structure hints, keyed by templateId. Unknown ids fall back to "general".
inline const QHash<QString, QString>& templates() {
    static const QHash<QString, QString> kTemplates = {
        {QStringLiteral("general"),
         QStringLiteral(
             "Structure the note with these Markdown H2 sections, in order:\n"
             "## Summary — 2-4 sentences of what the meeting was about and where it landed.\n"
             "## Key points — bullets of the substantive discussion.\n"
             "## Decisions — bullets of what was actually decided (omit the section if none).\n"
             "## Action items — bullets as `- [ ] Owner — task (due date if stated)`.")},
        {QStringLiteral("one-on-one"),
         QStringLiteral(
             "Structure the note with these Markdown H2 sections, in order:\n"
             "## Updates — what the report reported: progress, status, context.\n"
             "## Feedback — feedback given or received, in either direction.\n"
             "## Growth — career, skills, goals, longer-horizon topics.\n"
             "## Action items — bullets as `- [ ] Owner — task (due date if stated)`.\n"
             "Keep the tone factual; this is a private note, not a performance review.")},
        {QStringLiteral("standup"),
         QStringLiteral(
             "Structure the note with these Markdown H2 sections, in order:\n"
             "## Done — completed since the last standup.\n"
             "## In progress — currently being worked on.\n"
             "## Blockers — anything blocked, with who is needed to unblock it.\n"
             "Group bullets by person when speakers are identifiable. Be terse.")},
        {QStringLiteral("sales-call"),
         QStringLiteral(
             "Structure the note with these Markdown H2 sections, in order:\n"
             "## Company & contact — org, people, roles, size/stage if mentioned.\n"
             "## Pain points — problems the prospect described, in their own framing.\n"
             "## Objections — concerns raised about price, timing, fit, competitors.\n"
             "## Next steps — bullets as `- [ ] Owner — task (date if stated)`.")},
    };
    return kTemplates;
}

inline QString templateHint(const QString& templateId) {
    const auto& t = templates();
    const auto it = t.constFind(templateId);
    return it != t.constEnd() ? it.value() : t.value(QStringLiteral("general"));
}

inline QString systemPrompt(const QString& templateId) {
    return systemBase() + QStringLiteral("\n\n") + templateHint(templateId);
}

// Builds the user turn. cuesMd may be empty — then we ask for a faithful summary instead.
inline QString userMessage(const QString& cuesMd, const QString& transcriptText) {
    QString transcript = transcriptText;
    bool truncated = false;
    if (transcript.size() > kMaxTranscriptChars) {
        transcript = transcript.right(kMaxTranscriptChars);
        truncated = true;
    }

    QString msg;
    const QString cues = cuesMd.trimmed();
    if (cues.isEmpty()) {
        msg += QStringLiteral(
            "No cues were taken during this meeting. Produce a faithful, structured summary "
            "from the transcript alone, following the section structure above. Do not "
            "speculate about anything the transcript does not state.\n\n");
    } else {
        msg += QStringLiteral(
            "Here are my rough cues from the meeting. They are fragments, not prose — treat "
            "them as the outline of what I care about.\n\n"
            "<cues>\n") + cues + QStringLiteral("\n</cues>\n\n");
    }

    if (truncated) {
        msg += QStringLiteral(
            "The transcript below was truncated: only the final portion of a longer meeting "
            "is included. Do not claim to cover the parts you cannot see.\n\n");
    }
    msg += QStringLiteral("<transcript>\n") + transcript + QStringLiteral("\n</transcript>");
    return msg;
}

inline QString titleSystem() {
    return QStringLiteral(
        "You name meetings. Given a transcript, reply with a single title of 3 to 6 words "
        "that says what the meeting was about. No quotes, no trailing period, no preamble.");
}

inline QString titleUserMessage(const QString& transcriptText) {
    return QStringLiteral("Transcript (may be partial):\n\n")
           + transcriptText.left(kTitleTranscriptChars)
           + QStringLiteral("\n\nTitle:");
}

} // namespace prompts
} // namespace gromarch
