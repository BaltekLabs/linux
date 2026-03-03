import asyncio
import pygame
import sys
import logging
from typing import Optional

from core.event.event_bus import EventBus, Event, Priority
from core.llm.registry import ModelRegistry
from core.llm.monitor import ResourceMonitor
from core.llm.router import TaskRouter
from llm.inference import OllamaClient
from llm.adapters import OllamaAdapter
from ui.circle import Circle, CircleState, CircleConfig
from ui.text_input import TextInput
from core.llm.mock_llm import MockLLM
from core.llm.action_system import ActionExecutor
from ui.components.content_display import ContentDisplay

class VoiceOS:
    def __init__(self, width: int = None, height: int = None):
        # Setup logging
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
        )
        self.logger = logging.getLogger(__name__)
        
        # Initialize Pygame
        pygame.init()
        pygame.display.set_caption("VoiceOS")
        
        # Get the screen info
        screen_info = pygame.display.Info()
        self.width = width or int(screen_info.current_w * 0.8)  # 80% of screen width
        self.height = height or int(screen_info.current_h * 0.8)  # 80% of screen height
        
        # Create resizable window with double buffering
        self.screen = pygame.display.set_mode(
            (self.width, self.height),
            pygame.RESIZABLE | pygame.DOUBLEBUF
        )
        pygame.event.set_allowed([pygame.QUIT, pygame.KEYDOWN, pygame.MOUSEBUTTONDOWN, pygame.MOUSEWHEEL, pygame.VIDEORESIZE])
        
        self.clock = pygame.time.Clock()
        
        # Initialize UI components
        self.circle = Circle(self.screen, CircleConfig(
            radius=150,
            num_points=300,
            line_thickness=2
        ))
        
        # Initialize core components
        self.event_bus = EventBus()
        self.model_registry = ModelRegistry(self.event_bus)
        self.resource_monitor = ResourceMonitor(self.event_bus)
        self.task_router = TaskRouter(self.event_bus, self.model_registry)
        self.ollama_client = OllamaClient(self.event_bus)
        
        # Initialize adapters and executors
        self.ollama_adapter = OllamaAdapter(
            event_bus=self.event_bus,
            model_registry=self.model_registry,
            resource_monitor=self.resource_monitor,
            ollama_client=self.ollama_client
        )
        
        # Keep mock LLM and action executor for now
        self.mock_llm = MockLLM()
        self.action_executor = ActionExecutor(self.event_bus)
        
        self._setup_event_handlers()
        
        self.running = False
        self.event_queue = asyncio.Queue()
        self.text_input = TextInput(self.screen)
        self.content_display = ContentDisplay(self.screen, self.circle)
        
    def _setup_event_handlers(self):
        # Existing event handlers
        self.event_bus.subscribe("user_input", self._handle_user_input)
        self.event_bus.subscribe("text_input", self._handle_text_input)
        self.event_bus.subscribe("fetch_content", self._handle_content_fetch)
        self.event_bus.subscribe("display_message", self._handle_display_message)
        
        # New LLM system event handlers
        self.event_bus.subscribe("generation_chunk", self._handle_generation_chunk)
        self.event_bus.subscribe("model_status_update", self._handle_model_status)
        self.event_bus.subscribe("resource_warning", self._handle_resource_warning)
        
        self.logger.info("Event handlers setup complete")
    
    async def _handle_resize(self, new_width: int, new_height: int):
        """Handle window resize event"""
        try:
            # Enforce minimum dimensions
            self.width = max(800, new_width)
            self.height = max(600, new_height)
            
            # Store previous states
            previous_text_active = self.text_input.active
            previous_content = self.content_display.current_text
            
            # Update screen
            self.screen = pygame.display.set_mode(
                (self.width, self.height),
                pygame.RESIZABLE | pygame.DOUBLEBUF
            )
            
            # Update UI components
            self.circle.update_screen(self.screen)
            self.text_input.update_screen(self.screen)
            self.content_display.update_screen(self.screen)
            
            # Restore states
            if previous_text_active:
                self.text_input.active = True
            if previous_content:
                self.content_display.append_content(previous_content)
                
        except Exception as e:
            self.logger.error(f"Error handling resize: {str(e)}")
    
    async def _handle_text_input(self, event: Event):
        """Handle text input events using Ollama"""
        text = event.data
        
        # Set circle to listening state
        self.circle.set_state(CircleState.LISTENING)
        
        try:
            # Clear any previous content
            self.content_display.clear_content()
            
            # Create generation request
            generation_request = {
                "model": "mistral:latest",  # Updated to match registered model name exactly
                "prompt": text,
                "system": "You are VoiceOS, a natural language operating system. Interpret user commands and respond appropriately.",
                "stream": True
            }
            
            # Set to processing state
            self.circle.set_state(CircleState.PROCESSING)
            
            # Emit generation request
            await self.event_bus.emit(Event(
                type="generate_request",
                data=generation_request,
                priority=Priority.HIGH
            ))
            
        except Exception as e:
            self.logger.error(f"Error processing text input: {str(e)}")
            self.circle.set_state(CircleState.ERROR)
            await asyncio.sleep(1.0)
            self.circle.set_state(CircleState.IDLE)
    
    async def _handle_generation_chunk(self, event: Event):
        """Handle streaming text generation from LLMs"""
        response = event.data
        
        try:
            # Handle response chunks
            if isinstance(response, dict):
                chunk = response.get('response', '')
                created_at = response.get('created_at', '')
                done = response.get('done', False)
                
                if chunk and not done:
                    # Clear content on first chunk
                    if not hasattr(self, '_response_started'):
                        self.content_display.clear_content()
                        self._response_started = True
                    
                    # Update display with new chunk
                    self.content_display.append_content(chunk)
                    
                elif done:
                    # Reset for next response
                    if hasattr(self, '_response_started'):
                        delattr(self, '_response_started')
                    # Transition animation states
                    self.circle.set_state(CircleState.RESPONDING)
                    await asyncio.sleep(0.3)
                    self.circle.set_state(CircleState.IDLE)
                    
        except Exception as e:
            self.logger.error(f"Error handling generation chunk: {str(e)}")
            self.circle.set_state(CircleState.ERROR)
    
    async def _handle_model_status(self, event: Event):
        """Handle model status updates"""
        status = event.data.get('status')
        if status == 'PROCESSING':
            self.circle.set_state(CircleState.PROCESSING)
        elif status == 'READY':
            self.circle.set_state(CircleState.IDLE)
        elif status == 'ERROR':
            self.circle.set_state(CircleState.ERROR)
    
    async def _handle_resource_warning(self, event: Event):
        """Handle resource warning events"""
        warnings = event.data.get('warnings', [])
        for warning in warnings:
            self.logger.warning(f"Resource warning: {warning}")
            # Could add visual feedback here
    
    async def _handle_content_fetch(self, event: Event):
        """Handle content fetch events"""
        if event.data.get('content_type') == 'reddit':
            posts = event.data.get('posts', [])
            
            # Transform posts for display if needed
            display_posts = []
            for post in posts:
                display_post = {
                    'title': post['title'],
                    'score': post['score'],
                    'author': post['author'],
                    'comments': post['num_comments'],
                    'created': post['created'],
                    'flair': post.get('flair'),
                    'url': post['permalink'],
                    'content': post.get('content', '')
                }
                display_posts.append(display_post)
            
            # Update the content display
            self.content_display.set_content(display_posts)
            
            # Set circle to responding state briefly
            self.circle.set_state(CircleState.RESPONDING)
            await asyncio.sleep(0.5)
            
            # Return to idle
            self.circle.set_state(CircleState.IDLE)
    
    async def _handle_display_message(self, event: Event):
        """Handle displaying messages"""
        message = event.data.get('message')
        self.logger.info(f"System message: {message}")
        # Here you would implement the actual message display
        # For now, just print the message
    
    async def _handle_user_input(self, event: Event):
        self.logger.info("Starting state transitions")
        
        try:
            # First transition to LISTENING state briefly
            self.logger.info("→ LISTENING")
            self.circle.set_state(CircleState.LISTENING)
            await asyncio.sleep(0.5)
            
            # Then to PROCESSING state
            self.logger.info("→ PROCESSING")
            self.circle.set_state(CircleState.PROCESSING)
            await asyncio.sleep(1.0)
            
            # Finally back to IDLE
            self.logger.info("→ IDLE")
            self.circle.set_state(CircleState.IDLE)
            
        except Exception as e:
            self.logger.error(f"Error during state transition: {e}")
    
    async def _process_pygame_events(self):
        """Process all pygame events"""
        try:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    self.running = False
                elif event.type == pygame.VIDEORESIZE:
                    await self._handle_resize(event.w, event.h)
                elif event.type == pygame.MOUSEWHEEL and self.content_display.active:
                    self.content_display.handle_scroll(event.y)
                elif event.type == pygame.MOUSEBUTTONDOWN:
                    if event.button == 1:  # Left click
                        try:
                            if self.content_display.active:
                                self.content_display.handle_click(event.pos)
                        except Exception as e:
                            self.logger.error(f"Error handling mouse click: {e}")
                elif event.type == pygame.KEYDOWN:
                    if event.key == pygame.K_ESCAPE:
                        self.running = False
                    elif event.key == pygame.K_SPACE and not self.text_input.active:
                        # Only trigger space event if not typing
                        self.logger.info("Space key pressed - emitting event")
                        try:
                            await self.event_bus.emit(Event(
                                type="user_input",
                                data="space_pressed",
                                priority=Priority.HIGH
                            ))
                            self.logger.info("Event emitted successfully")
                        except Exception as e:
                            self.logger.error(f"Error emitting event: {e}")
                    
                    # Handle text input
                    try:
                        completed_text = self.text_input.handle_event(event)
                        if completed_text:
                            self.logger.info(f"Text input completed: {completed_text}")
                            await self.event_bus.emit(Event(
                                type="text_input",
                                data=completed_text,
                                priority=Priority.HIGH
                            ))
                    except Exception as e:
                        self.logger.error(f"Error handling text input: {e}")
                        
        except Exception as e:
            self.logger.error(f"Error in event processing: {e}")
            # Continue running even if there's an error in event processing
            pass
            
    async def _update(self):
        """Update game state"""
        dt = self.clock.get_time() / 1000.0
        self.circle.update(dt)
        self.text_input.update(dt)
        self.content_display.update(dt)
    
    async def _draw(self):
        """Draw the current frame"""    
        self.screen.fill((0, 0, 0))
        self.circle.draw()
        self.content_display.draw()
        self.text_input.draw()
        pygame.display.flip()
    
    async def initialize_llm_system(self):
        """Initialize the LLM system components"""
        try:
            # Start core services
            await self.event_bus.start()
            await self.resource_monitor.start()
            await self.task_router.start()
            
            # Initialize Ollama integration
            await self.ollama_adapter.initialize()
            
            # Initialize action executor
            await self.action_executor.initialize()
            
            self.logger.info("LLM system initialized successfully")
            
        except Exception as e:
            self.logger.error(f"Failed to initialize LLM system: {str(e)}")
            raise
    
    async def cleanup_llm_system(self):
        """Cleanup LLM system components"""
        try:
            await self.task_router.stop()
            await self.resource_monitor.stop()
            await self.ollama_client.cleanup()
            await self.action_executor.cleanup()
            await self.event_bus.stop()
            
            self.logger.info("LLM system cleanup complete")
            
        except Exception as e:
            self.logger.error(f"Error during LLM system cleanup: {str(e)}")
    
    async def run(self):
        self.running = True
        
        try:
            # Initialize LLM system
            await self.initialize_llm_system()
            
            # Main loop
            while self.running:
                await self._process_pygame_events()
                await self._update()
                await self._draw()
                await asyncio.sleep(0)
                self.clock.tick(60)
                
        except Exception as e:
            self.logger.error(f"Runtime error: {str(e)}")
        finally:
            self.logger.info("Shutting down")
            await self.cleanup_llm_system()
            pygame.quit()

def main():
    app = VoiceOS()
    asyncio.run(app.run())

if __name__ == "__main__":
    main()