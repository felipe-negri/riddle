package org.riddle.diary.settings

enum class ProviderKind(val label: String) {
    OPENAI_COMPATIBLE("OpenAI-compatible"),
    OLLAMA("Ollama"),
    ANTHROPIC("Anthropic (Claude)"),
    GEMINI("Gemini"),
    COPILOT("GitHub Copilot (experimental)"),
}
