"""
Skill registry — stores skills and selects the best one for a given query.
"""

import logging
from typing import Dict, List, Optional

from skills.loader import Skill, SkillLoader

logger = logging.getLogger(__name__)


class SkillRegistry:
    def __init__(self):
        self._skills: Dict[str, Skill] = {}

    def register(self, skill: Skill) -> None:
        self._skills[skill.name] = skill

    def load_from_dirs(self, *dirs: str) -> int:
        loader = SkillLoader()
        for d in dirs:
            loader.add_path(d)
        loaded = loader.load_all()
        for skill in loaded:
            self.register(skill)
        logger.info("Loaded %d skills from %d directories", len(loaded), len(dirs))
        return len(loaded)

    def get(self, name: str) -> Optional[Skill]:
        return self._skills.get(name)

    def list_skills(self) -> List[str]:
        return list(self._skills.keys())

    def all_skills(self) -> List[Skill]:
        return list(self._skills.values())

    def match(self, user_input: str, threshold: float = 0.0) -> Optional[Skill]:
        """Return the best-matching skill for the input, or None."""
        best: Optional[Skill] = None
        best_score = threshold

        for skill in self._skills.values():
            score = skill.match_score(user_input)
            if score > best_score:
                best_score = score
                best = skill

        if best:
            logger.debug("Matched skill '%s' (score=%.2f) for input: %s", best.name, best_score, user_input[:60])
        return best

    def get_system_prompt(self, skill_name: Optional[str] = None, base_prompt: str = "") -> str:
        """Compose the system prompt: base + skill-specific."""
        parts = []
        if base_prompt:
            parts.append(base_prompt)

        if skill_name:
            skill = self._skills.get(skill_name)
            if skill:
                parts.append(f"\n## Active Skill: {skill.name}\n{skill.system_prompt}")

        return "\n".join(parts)

    def get_tools_for_skill(self, skill_name: str) -> List[str]:
        """Return the tool names a skill requests."""
        skill = self._skills.get(skill_name)
        return skill.tools if skill else []

    def summary(self) -> str:
        lines = [f"Loaded {len(self._skills)} skills:"]
        for s in sorted(self._skills.values(), key=lambda x: x.name):
            lines.append(f"  [{s.name}] {s.description} (triggers: {', '.join(s.triggers[:4])})")
        return "\n".join(lines)
