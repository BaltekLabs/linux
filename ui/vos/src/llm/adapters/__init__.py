# src/llm/adapters/__init__.py

from .ollama_adapter import OllamaAdapter
from .exceptions import ResourceError

__all__ = ['OllamaAdapter', 'ResourceError']