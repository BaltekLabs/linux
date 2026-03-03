import pygame
import math
from typing import List, Dict, Any

# src/ui/components/content_display.py

class ContentDisplay:
    def __init__(self, screen: pygame.Surface, circle):
        # Initialize basic attributes first
        self.screen = screen
        self.circle = circle
        self.content = []
        self.font = pygame.font.Font(None, 24)
        self.title_font = pygame.font.Font(None, 28)
        self.active = False
        self.selected_item = None
        self.current_text = ""
        
        # Animation and layout parameters
        self.expansion_progress = 0.0
        self.target_expansion = 0.0
        self.expansion_speed = 2.0
        self.line_height = 25
        self.scroll_offset = 0
        self.post_padding = 10
        
        # Update screen and calculate dimensions
        self.update_screen(screen)

    def update_screen(self, screen):
        """Update screen reference and recalculate dimensions"""
        self.screen = screen
        self.width, self.height = screen.get_size()
        self.post_width = int(self.width * 0.6)
        self.post_height = int(self.height * 0.7)
        self.max_lines_visible = self.post_height // self.line_height
        
        # Recalculate positioning
        center_x = self.width // 2
        center_y = self.height // 2
        self.start_x = center_x + self.circle.config.radius + 20
        self.start_y = center_y - (self.post_height // 2)

    def append_content(self, text: str):
        """Append text content to the display"""
        # Clean up the text - remove excessive whitespace and normalize line endings
        text = ' '.join(text.split())
        if not text:
            return
            
        # Append to current text with proper spacing
        if self.current_text:
            self.current_text += ' ' + text
        else:
            self.current_text = text
        
        # Format content for display
        self.content = [{
            'text': self.current_text,
            'lines': self._wrap_text(self.current_text)
        }]
        self.active = True
        self.target_expansion = 1.0
        
        # Update position
        center_x = self.screen.get_width() // 2
        center_y = self.screen.get_height() // 2
        self.start_x = center_x + self.circle.config.radius + 20
        self.start_y = center_y - (self.post_height // 2)
        
        # Auto-scroll to show new content
        if len(self.content[0]['lines']) > self.max_lines_visible:
            self.scroll_offset = len(self.content[0]['lines']) - self.max_lines_visible

    def clear_content(self):
        """Clear all content and reset display"""
        self.content = []
        self.current_text = ""
        self.active = False
        self.selected_item = None
        self.target_expansion = 0.0
        self.scroll_offset = 0

    def update(self, dt: float):
        """Update animation state"""
        if self.active:
            if self.expansion_progress < self.target_expansion:
                self.expansion_progress = min(1.0, self.expansion_progress + self.expansion_speed * dt)
            elif self.expansion_progress > self.target_expansion:
                self.expansion_progress = max(0.0, self.expansion_progress - self.expansion_speed * dt)

    def handle_scroll(self, scroll_amount: int):
        """Handle mouse wheel scrolling"""
        if self.content and 'lines' in self.content[0]:
            max_scroll = max(0, len(self.content[0]['lines']) - self.max_lines_visible)
            self.scroll_offset = max(0, min(self.scroll_offset - scroll_amount, max_scroll))

    def _wrap_text(self, text: str) -> list:
        """Wrap text to fit within post width"""
        wrapped_lines = []
        words = text.split()
        current_line = ""
        
        for word in words:
            test_line = current_line + " " + word if current_line else word
            test_surface = self.font.render(test_line, True, (255, 255, 255))
            
            if test_surface.get_width() > self.post_width - 40:  # Added more padding
                if current_line:
                    wrapped_lines.append(current_line)
                    current_line = word
                else:
                    wrapped_lines.append(word)
                    current_line = ""
            else:
                current_line = test_line
        
        if current_line:
            wrapped_lines.append(current_line)
        
        return wrapped_lines

    def _get_post_rect(self, index: int) -> pygame.Rect:
        """Get the rectangle for a post"""
        x = self.start_x
        y = self.start_y
        return pygame.Rect(x, y, self.post_width * self.expansion_progress, self.post_height)

    def draw(self):
        """Draw the content display"""
        if not self.active or not self.content or self.expansion_progress <= 0:
            return
        
        # Create a surface for the posts
        posts_surface = pygame.Surface(self.screen.get_size(), pygame.SRCALPHA)
        
        # Draw each post
        for i, item in enumerate(self.content):
            post_rect = self._get_post_rect(i)
            self._draw_post(posts_surface, item, post_rect, item == self.selected_item)
        
        # Blend the posts surface onto the screen
        self.screen.blit(posts_surface, (0, 0))

    def _draw_post(self, surface: pygame.Surface, item: Dict[str, Any], rect: pygame.Rect, selected: bool):
        """Draw a single post"""
        # Draw post background with transparency
        bg_surface = pygame.Surface((rect.width, rect.height), pygame.SRCALPHA)
        pygame.draw.rect(bg_surface, (40, 70, 120, 200), bg_surface.get_rect(), border_radius=10)
        surface.blit(bg_surface, rect)
        
        # Draw text content
        if 'text' in item and 'lines' in item:
            lines = item['lines']
            visible_lines = lines[self.scroll_offset:self.scroll_offset + self.max_lines_visible]
            y_offset = rect.y + 10
            
            for line in visible_lines:
                text_surface = self.font.render(line, True, (255, 255, 255))
                surface.blit(text_surface, (rect.x + 10, y_offset))
                y_offset += self.line_height