import pygame
import math
from typing import Tuple, Optional

class TextInput:
    def __init__(self, screen: pygame.Surface):
        # Initialize base attributes first
        self.text = ""
        self.active = False
        self.alpha = 0
        self.current_y = 0
        self.target_y = 0
        
        # Animation properties
        self.animation_speed = 300
        self.fade_speed = 2.5
        
        # Text properties
        self.font = pygame.font.Font(None, 48)
        
        # Finally, update screen and positions
        self.update_screen(screen)

    def update_screen(self, screen: pygame.Surface):
        """Update screen reference and recalculate dimensions"""
        self.screen = screen
        self.width, self.height = screen.get_size()
        self._update_positions()

    def _update_positions(self):
        """Update position calculations based on current dimensions"""
        self.target_y = self.height * 0.75
        if not self.active:
            self.current_y = self.height
        else:
            # Keep relative position when resizing while active
            self.current_y = self.target_y

    def handle_event(self, event: pygame.event.Event) -> Optional[str]:
        """Handle keyboard events. Returns completed text when Enter is pressed."""
        if event.type == pygame.KEYDOWN:
            if not self.active and event.unicode.isprintable():
                # First keypress activates input
                self.active = True
                self.text = event.unicode
            elif self.active:
                if event.key == pygame.K_RETURN:
                    completed_text = self.text
                    self.text = ""
                    self.active = False
                    return completed_text
                elif event.key == pygame.K_BACKSPACE:
                    self.text = self.text[:-1]
                elif event.unicode.isprintable():
                    self.text += event.unicode
        return None

    def update(self, dt: float):
        """Update animation state"""
        if self.active:
            # Fade in
            self.alpha = min(255, self.alpha + self.fade_speed * dt * 255)
            # Slide up
            distance = self.target_y - self.current_y
            if abs(distance) > 1:
                self.current_y += min(
                    self.animation_speed * dt,
                    abs(distance)
                ) * math.copysign(1, distance)
        else:
            # Fade out
            self.alpha = max(0, self.alpha - self.fade_speed * dt * 255)
            # Reset position when fully transparent
            if self.alpha == 0:
                self.current_y = self.height

    def draw(self):
        """Render text input"""
        if self.alpha > 0:
            text_surface = self.font.render(self.text + ('|' if self.active else ''), True, (255, 255, 255))
            text_surface.set_alpha(int(self.alpha))
            
            # Center text horizontally
            text_rect = text_surface.get_rect(
                centerx=self.width // 2,
                centery=self.current_y
            )
            self.screen.blit(text_surface, text_rect)