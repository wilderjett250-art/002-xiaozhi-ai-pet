from __future__ import annotations

import json
import os
import re
import urllib.request
from dataclasses import dataclass


@dataclass(frozen=True)
class AmbientDecision:
    action: str
    response: str = ""
    reason: str = ""
    engine: str = "rules"


class AmbientDecisionEngine:
    def __init__(self) -> None:
        self.base_url = os.getenv("PET_CLOUD_LLM_BASE_URL", "").strip().rstrip("/")
        self.api_key = os.getenv("PET_CLOUD_LLM_API_KEY", "").strip()
        self.model = os.getenv("PET_CLOUD_LLM_MODEL", "").strip()

    def status(self) -> dict:
        configured = bool(self.base_url and self.model)
        return {"engine": "openai-compatible" if configured else "local-rules", "configured": configured}

    def decide(self, text: str, recent_chats: list[dict] | None = None) -> AmbientDecision:
        cleaned = re.sub(r"\s+", "", text).strip("，。！？,.!? ")
        if len(cleaned) < 4:
            return AmbientDecision("ignore", reason="内容过短")
        if self.base_url and self.model:
            try:
                return self._decide_with_llm(cleaned, recent_chats or [])
            except Exception:
                pass
        return self._decide_with_rules(cleaned)

    def _decide_with_rules(self, text: str) -> AmbientDecision:
        emotional = {
            "累": "听起来你有点累，要不要和我说说今天发生了什么？",
            "烦": "你好像有点烦，是什么事情让你不舒服呀？",
            "难过": "我听见你有点难过，想和我说说发生什么了吗？",
            "生气": "听起来你有点生气，是遇到什么事情了吗？",
            "开心": "听起来你很开心，发生什么好事情啦？",
            "紧张": "你好像有点紧张，是在担心什么事情吗？",
            "纠结": "你听起来有点纠结，是哪两个选择让你拿不定主意呀？",
        }
        for marker, response in emotional.items():
            if marker in text:
                return AmbientDecision("ask", response, f"检测到情绪词：{marker}")

        direct_markers = ("小喵", "喵喵", "小智", "你觉得", "你知道", "你说")
        if any(marker in text for marker in direct_markers):
            excerpt = text[:18]
            return AmbientDecision("ask", f"我听到你提到“{excerpt}”，你想和我聊聊这个吗？", "疑似直接交流")

        personal_markers = ("我今天", "我刚才", "我最近", "我现在", "我觉得", "我想", "我不知道", "怎么办")
        if any(marker in text for marker in personal_markers):
            excerpt = text[:18]
            return AmbientDecision("ask", f"你刚才说“{excerpt}”，后来怎么样了？", "检测到个人叙述")
        return AmbientDecision("ignore", reason="没有明确的个人内容或交流意图")

    def _decide_with_llm(self, text: str, recent_chats: list[dict]) -> AmbientDecision:
        context = "\n".join(
            f"{item.get('role', '')}: {item.get('content', '')[:200]}" for item in recent_chats[-6:]
        )
        messages = [
            {
                "role": "system",
                "content": (
                    "你是猫咪桌宠的环境语音决策器。只在用户明显表达个人情绪、经历、困惑，"
                    "或明确和桌宠说话时回应；电视、旁人聊天、无意义片段必须忽略。"
                    "输出严格 JSON：action 只能是 ignore、ask、reply；response 最多50个汉字；reason 简短。"
                ),
            },
            {"role": "user", "content": f"最近对话：\n{context or '无'}\n\n环境识别文字：{text}"},
        ]
        body = json.dumps(
            {"model": self.model, "messages": messages, "temperature": 0.2, "response_format": {"type": "json_object"}},
            ensure_ascii=False,
        ).encode("utf-8")
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        request = urllib.request.Request(f"{self.base_url}/chat/completions", data=body, headers=headers, method="POST")
        with urllib.request.urlopen(request, timeout=20) as response:
            payload = json.loads(response.read().decode("utf-8"))
        content = payload["choices"][0]["message"]["content"]
        result = json.loads(content)
        action = result.get("action", "ignore")
        response_text = str(result.get("response", "")).strip()[:80]
        reason = str(result.get("reason", "")).strip()[:160]
        if action not in {"ignore", "ask", "reply"} or (action != "ignore" and not response_text):
            return AmbientDecision("ignore", reason="模型决策格式无效", engine="openai-compatible")
        return AmbientDecision(action, response_text, reason, engine="openai-compatible")


decision_engine = AmbientDecisionEngine()
