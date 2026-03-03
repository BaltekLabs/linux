from __future__ import annotations

import pygame
import math
import random
import colorsys
from dataclasses import dataclass
from typing import Tuple, Optional, List
from enum import Enum, auto

class CircleState(Enum):
    IDLE = auto()
    LISTENING = auto()
    PROCESSING = auto()
    RESPONDING = auto()
    ERROR = auto()

@dataclass
class CircleConfig:
    radius: int = 150
    num_points: int = 300
    line_thickness: int = 2
    amplitude_base: float = 15
    base_speed: float = 0.15  # Increased from 0.08

class Circle:
    def __init__(self, screen: pygame.Surface, config: Optional[CircleConfig] = None):
        
        self.screen = screen
        self.config = config or CircleConfig()
        self.width, self.height = screen.get_size()
        self.center = (self.width // 2, self.height // 2)
        self.state = CircleState.IDLE
        self.time = 0
        self.base_color = (0, 150, 255)
        
        # Animation parameters
        self._setup_animation_params()

        self.current_state = CircleState.IDLE
        self.target_state = CircleState.IDLE
        self.transition_progress = 1.0  # 1.0 means fully in current state
        self.transition_speed = 0.8 #Seconds for complete transition

    def _setup_animation_params(self):
        """Initialize animation parameters"""
        self.noise_offsets = [
            (random.uniform(0, 1000), random.uniform(0, 1000))
            for _ in range(self.config.num_points)
        ]

    def _get_state_intensity(self, state: CircleState) -> float:
        """Get movement intensity for current state"""
        intensities = {
            CircleState.IDLE: 1.0,
            CircleState.LISTENING: 1.8,
            CircleState.PROCESSING: 2.5,
            CircleState.RESPONDING: 1.5,
            CircleState.ERROR: 3.0
        }
        return intensities[state]
    
    def _get_organic_deformation(self, angle: float, t: float, intensity: float) -> float:
        """Generate organic, fluid-like deformation"""
        # Get intensities for both current and target states
        current_intensity = self._get_state_intensity(self.current_state)
        target_intensity = self._get_state_intensity(self.target_state)
        
        # Blend intensities
        blended_intensity = self._lerp(
            current_intensity, 
            target_intensity, 
            self.transition_progress
        ) * intensity

        # Slow undulating base movement
        base_wave = math.sin(angle * 2 + t * 1.0) * 10  # 0.5 -> 1.0
    
        ripples = (
            math.sin(angle * 4 + t * 2.4) * 5 +  # 1.2 -> 2.4
            math.sin(angle * 3 - t * 1.6) * 4    # 0.8 -> 1.6
        )
        
        pulse = math.sin(t * 0.6) * 8  # 0.3 -> 0.6
            
        return (base_wave + ripples + pulse) * blended_intensity

    def _generate_points(self, t: float) -> List[Tuple[float, float]]:
        points = []
        base_intensity = 1.0  # Base intensity multiplier
        
        for i in range(self.config.num_points):
            angle = i * 2 * math.pi / self.config.num_points
            
            # Get organic deformation with blended state intensities
            deform = self._get_organic_deformation(angle, t, base_intensity)
            
            # Add some noise for more natural movement
            noise = math.sin(t + i * 0.2) * 3
            
            # Calculate radius with blended effects
            current_radius = self.config.radius + deform + noise
            
            x = self.center[0] + current_radius * math.cos(angle)
            y = self.center[1] + current_radius * math.sin(angle)
            points.append((x, y))

        # Multiple passes of smoothing for extra fluidity
        smoothed = points
        for _ in range(4):
            smoothed = self._interpolate_points(smoothed, 0.18)
        
        return smoothed

    def _interpolate_points(self, points: List[Tuple[float, float]], factor: float = 0.3) -> List[Tuple[float, float]]:
        """Smooth the circle points using interpolation"""
        smoothed = []
        num = len(points)
        for i in range(num):
            prev_2 = points[(i - 2) % num]
            prev_1 = points[(i - 1) % num]
            curr = points[i]
            next_1 = points[(i + 1) % num]
            next_2 = points[(i + 2) % num]
            
            x = curr[0] * (1 - factor) + factor * (
                prev_2[0] * 0.1 + 
                prev_1[0] * 0.2 + 
                next_1[0] * 0.2 + 
                next_2[0] * 0.1)
            
            y = curr[1] * (1 - factor) + factor * (
                prev_2[1] * 0.1 + 
                prev_1[1] * 0.2 + 
                next_1[1] * 0.2 + 
                next_2[1] * 0.1)
            
            smoothed.append((x, y))
        return smoothed

    def _lerp(self, start: float, end: float, t: float) -> float:
        """Linear interpolation between two values"""
        return start + (end - start) * t

    def update(self, dt: float):
        """Update animation state"""
        self.time += dt * self.config.base_speed

        # Update state transition
        if self.transition_progress < 1.0:
            self.transition_progress = min(1.0, 
                self.transition_progress + dt / self.transition_speed)

    def draw(self):
        """Draw the circle"""
        points = self._generate_points(self.time)
        
        # Draw filled circle
        pygame.draw.polygon(self.screen, self.base_color, points)
        
        # Draw white outline
        pygame.draw.lines(self.screen, (255, 255, 255), True, points, self.config.line_thickness)

    def set_state(self, state: CircleState):
        """Smoothly transition to a new state"""
        if state != self.target_state:
            self.current_state = self.target_state
            self.target_state = state
            self.transition_progress = 0.0

    def update_screen(self, screen: pygame.Surface):
        """Update screen reference and recalculate dimensions"""
        self.screen = screen
        self.width, self.height = screen.get_size()
        self.center = (self.width // 2, self.height // 2)
        # Optionally adjust radius based on screen size
        self.config.radius = min(self.width, self.height) // 4