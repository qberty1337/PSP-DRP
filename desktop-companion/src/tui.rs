//! TUI (Terminal User Interface) Module
//!
//! Provides a real-time terminal interface for monitoring PSP status,
//! Discord connection, and displaying game icons as ASCII art.

use std::io::{self, Stdout};
use std::net::SocketAddr;
use std::time::{Duration, Instant};

use crossterm::{
    event::{self, Event, KeyCode, KeyEventKind},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{
    backend::CrosstermBackend,
    layout::{Alignment, Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style, Stylize},
    text::{Line, Span, Text},
    widgets::{Block, Borders, List, ListItem, Padding, Paragraph},
    Frame, Terminal,
};

use crate::protocol::{GameInfo, PspState};

/// Maximum number of log messages to keep
const MAX_LOG_MESSAGES: usize = 50;

/// TUI event types
#[derive(Debug, Clone)]
pub enum TuiEvent {
    /// User wants to quit
    Quit,
}

/// State shared between the server and TUI
#[derive(Debug, Clone, Default)]
pub struct TuiState {
    /// Connected PSP name
    pub psp_name: Option<String>,
    /// PSP IP address
    pub psp_addr: Option<SocketAddr>,
    /// Battery level (0-100)
    pub battery_level: u8,
    /// WiFi strength (0-100)
    pub wifi_strength: u8,
    /// Current game info
    pub current_game: Option<GameInfo>,
    /// Discord connection status
    pub discord_connected: bool,
    /// Current ASCII art for display
    pub ascii_art: Option<String>,
    /// Log messages
    pub log_messages: Vec<LogEntry>,
    /// Session start time
    pub session_start: Option<Instant>,
    /// Application status message
    pub status_message: String,
    /// Top 3 most played games (title, formatted playtime)
    pub top_played: Vec<(String, String)>,
}

/// A log entry with timestamp and content
#[derive(Debug, Clone)]
pub struct LogEntry {
    pub timestamp: String,
    pub message: String,
    pub level: LogLevel,
}

/// Log level for coloring
#[derive(Debug, Clone, Copy)]
pub enum LogLevel {
    Info,
    Warn,
    Error,
    Success,
}

impl TuiState {
    /// Create new TUI state
    pub fn new() -> Self {
        Self {
            status_message: "Starting up...".to_string(),
            ..Default::default()
        }
    }

    /// Add a log message
    pub fn log(&mut self, level: LogLevel, message: &str) {
        let timestamp = chrono::Local::now().format("%H:%M:%S").to_string();
        self.log_messages.push(LogEntry {
            timestamp,
            message: message.to_string(),
            level,
        });

        // Keep only the last N messages
        if self.log_messages.len() > MAX_LOG_MESSAGES {
            self.log_messages.remove(0);
        }
    }

    /// Log info message
    pub fn log_info(&mut self, message: &str) {
        self.log(LogLevel::Info, message);
    }

    /// Log warning message
    pub fn log_warn(&mut self, message: &str) {
        self.log(LogLevel::Warn, message);
    }

    /// Log error message
    pub fn log_error(&mut self, message: &str) {
        self.log(LogLevel::Error, message);
    }

    /// Log success message
    pub fn log_success(&mut self, message: &str) {
        self.log(LogLevel::Success, message);
    }

    /// Get formatted session duration
    pub fn get_session_duration(&self) -> String {
        if let Some(start) = self.session_start {
            let elapsed = start.elapsed();
            let hours = elapsed.as_secs() / 3600;
            let minutes = (elapsed.as_secs() % 3600) / 60;
            let seconds = elapsed.as_secs() % 60;
            format!("{:02}:{:02}:{:02}", hours, minutes, seconds)
        } else {
            "--:--:--".to_string()
        }
    }
}

/// Terminal wrapper for the TUI
pub struct Tui {
    terminal: Terminal<CrosstermBackend<Stdout>>,
}

impl Tui {
    /// Initialize the TUI
    pub fn new() -> io::Result<Self> {
        enable_raw_mode()?;
        let mut stdout = io::stdout();
        execute!(stdout, EnterAlternateScreen)?;
        let backend = CrosstermBackend::new(stdout);
        let terminal = Terminal::new(backend)?;
        Ok(Self { terminal })
    }

    /// Restore terminal to normal mode
    pub fn restore(&mut self) -> io::Result<()> {
        disable_raw_mode()?;
        execute!(self.terminal.backend_mut(), LeaveAlternateScreen)?;
        self.terminal.show_cursor()?;
        Ok(())
    }

    /// Draw the TUI
    pub fn draw(&mut self, state: &TuiState) -> io::Result<()> {
        self.terminal.draw(|frame| {
            render_ui(frame, state);
        })?;
        Ok(())
    }

    /// Handle keyboard events
    pub fn handle_events(&mut self, timeout: Duration) -> io::Result<Option<TuiEvent>> {
        if event::poll(timeout)? {
            if let Event::Key(key) = event::read()? {
                if key.kind == KeyEventKind::Press {
                    match key.code {
                        KeyCode::Char('q') | KeyCode::Char('Q') | KeyCode::Esc => {
                            return Ok(Some(TuiEvent::Quit));
                        }
                        _ => {}
                    }
                }
            }
        }
        Ok(None)
    }
}

impl Drop for Tui {
    fn drop(&mut self) {
        let _ = self.restore();
    }
}

/// Main UI rendering function
fn render_ui(frame: &mut Frame, state: &TuiState) {
    let area = frame.area();

    // Create outer border for the entire app
    let outer_block = Block::default()
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(60, 60, 80)))
        .style(Style::default().bg(Color::Rgb(15, 15, 25)));
    
    frame.render_widget(outer_block.clone(), area);
    let inner_area = outer_block.inner(area);

    // Main vertical layout
    let main_chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(2),   // Header
            Constraint::Length(15),  // Content (fixed)
            Constraint::Min(8),      // Logs (grows with window)
            Constraint::Length(1),   // Footer
        ])
        .split(inner_area);

    // Render sections
    render_header(frame, main_chunks[0], state);
    render_content(frame, main_chunks[1], state);
    render_logs(frame, main_chunks[2], state);
    render_footer(frame, main_chunks[3], state);
}

/// Render the header bar
fn render_header(frame: &mut Frame, area: Rect, state: &TuiState) {
    // Split header into left and right sections
    // Give the right side (most played) as much space as possible
    let header_chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Length(42),     // Left: title and status (fixed width)
            Constraint::Min(0),         // Right: most played (gets all remaining space)
        ])
        .split(area);

    // Left side: Title and status indicators
    let discord_indicator = if state.discord_connected {
        Span::styled("● DISCORD", Style::default().fg(Color::Green).bold())
    } else {
        Span::styled("○ DISCORD", Style::default().fg(Color::Red))
    };

    let psp_indicator = if state.psp_name.is_some() {
        Span::styled("● PSP", Style::default().fg(Color::Green).bold())
    } else {
        Span::styled("○ PSP", Style::default().fg(Color::DarkGray))
    };

    let left_content = Line::from(vec![
        Span::styled(" PSP ", Style::default().fg(Color::Black).bg(Color::Magenta).bold()),
        Span::styled(" Discord Rich Presence ", Style::default().fg(Color::Magenta).bold()),
        Span::raw("  "),
        discord_indicator,
        Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
        psp_indicator,
    ]);

    let left_header = Paragraph::new(left_content)
        .block(
            Block::default()
                .borders(Borders::BOTTOM)
                .border_style(Style::default().fg(Color::Rgb(60, 60, 80)))
                .padding(Padding::horizontal(1))
        )
        .alignment(Alignment::Left);

    frame.render_widget(left_header, header_chunks[0]);


    // Right side: Top most played games (compact display)
    let right_content = if !state.top_played.is_empty() {
        let count = state.top_played.len();
        
        // Use short label to maximize space for game names
        let label = match count {
            1 => "★ ".to_string(),
            n => format!("★ Top {}: ", n),
        };
        
        let mut spans = vec![
            Span::styled(label, Style::default().fg(Color::Yellow)),
        ];
        
        for (i, (title, playtime)) in state.top_played.iter().enumerate() {
            if i > 0 {
                spans.push(Span::styled(" | ", Style::default().fg(Color::DarkGray)));
            }
            
            // Only show number prefix if more than 1 game
            if count > 1 {
                spans.push(Span::styled(format!("{}.", i + 1), Style::default().fg(Color::Cyan)));
            }
            spans.push(Span::styled(title.clone(), Style::default().fg(Color::White).bold()));
            spans.push(Span::styled(format!(" ({})", playtime), Style::default().fg(Color::DarkGray)));
        }
        
        Line::from(spans)
    } else {
        Line::from(vec![
            Span::styled("★ ", Style::default().fg(Color::DarkGray)),
            Span::styled("No data yet", Style::default().fg(Color::DarkGray).italic()),
        ])
    };

    let right_header = Paragraph::new(right_content)
        .block(
            Block::default()
                .borders(Borders::BOTTOM)
                .border_style(Style::default().fg(Color::Rgb(60, 60, 80)))
                .padding(Padding::horizontal(1))
        )
        .alignment(Alignment::Right);

    frame.render_widget(right_header, header_chunks[1]);
}

/// Render the main content area
fn render_content(frame: &mut Frame, area: Rect, state: &TuiState) {
    // Split into left (status) and right (game icon) panels
    let content_chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Min(35),       // Status panel (minimum width)
            Constraint::Percentage(55), // Game icon panel
        ])
        .split(area);

    render_status_panel(frame, content_chunks[0], state);
    render_game_panel(frame, content_chunks[1], state);
}

/// Render status panel
fn render_status_panel(frame: &mut Frame, area: Rect, state: &TuiState) {
    let block = Block::default()
        .title(Span::styled(" Status ", Style::default().fg(Color::Cyan).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)))
        .padding(Padding::new(1, 1, 1, 0));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    // Vertical layout for status items
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(2),  // PSP Connection
            Constraint::Length(1),  // Spacer
            Constraint::Length(2),  // Current Game
            Constraint::Length(2),  // Game ID
            Constraint::Length(2),  // State
            Constraint::Length(1),  // Spacer
            Constraint::Length(1),  // Session
            Constraint::Min(0),     // Fill
        ])
        .split(inner);

    // PSP Connection Status
    let psp_line = if let Some(ref name) = state.psp_name {
        let addr_str = state.psp_addr.map_or(String::new(), |a| format!(" @ {}", a.ip()));
        vec![
            Line::from(vec![
                Span::styled("PSP: ", Style::default().fg(Color::DarkGray)),
                Span::styled(name.clone(), Style::default().fg(Color::White).bold()),
                Span::styled(addr_str, Style::default().fg(Color::DarkGray)),
            ]),
        ]
    } else {
        vec![
            Line::from(vec![
                Span::styled("PSP: ", Style::default().fg(Color::DarkGray)),
                Span::styled("Waiting...", Style::default().fg(Color::Yellow).add_modifier(Modifier::SLOW_BLINK)),
            ]),
        ]
    };
    frame.render_widget(Paragraph::new(psp_line), chunks[0]);

    // Current Game
    let game_lines = if let Some(ref game) = state.current_game {
        let title = if game.title.is_empty() {
            game.state.as_str().to_string()
        } else if game.title.len() > 28 {
            format!("{}...", &game.title[..25])
        } else {
            game.title.clone()
        };
        
        vec![
            Line::from(vec![
                Span::styled("Game: ", Style::default().fg(Color::DarkGray)),
                Span::styled(title, Style::default().fg(Color::White).bold()),
            ]),
        ]
    } else {
        vec![
            Line::from(vec![
                Span::styled("Game: ", Style::default().fg(Color::DarkGray)),
                Span::styled("None", Style::default().fg(Color::DarkGray).italic()),
            ]),
        ]
    };
    frame.render_widget(Paragraph::new(game_lines), chunks[2]);

    // Game ID
    let id_lines = if let Some(ref game) = state.current_game {
        vec![
            Line::from(vec![
                Span::styled("ID:   ", Style::default().fg(Color::DarkGray)),
                Span::styled(&game.game_id, Style::default().fg(Color::Cyan)),
            ]),
        ]
    } else {
        vec![
            Line::from(vec![
                Span::styled("ID:   ", Style::default().fg(Color::DarkGray)),
                Span::styled("---", Style::default().fg(Color::DarkGray)),
            ]),
        ]
    };
    frame.render_widget(Paragraph::new(id_lines), chunks[3]);

    // State
    let state_lines = if let Some(ref game) = state.current_game {
        let state_color = match game.state {
            PspState::Xmb => Color::Blue,
            PspState::Game => Color::Green,
            PspState::Homebrew => Color::Magenta,
            PspState::Video => Color::Cyan,
            PspState::Music => Color::Yellow,
        };
        vec![
            Line::from(vec![
                Span::styled("Mode: ", Style::default().fg(Color::DarkGray)),
                Span::styled(game.state.as_str(), Style::default().fg(state_color).bold()),
            ]),
        ]
    } else {
        vec![
            Line::from(vec![
                Span::styled("Mode: ", Style::default().fg(Color::DarkGray)),
                Span::styled("---", Style::default().fg(Color::DarkGray)),
            ]),
        ]
    };
    frame.render_widget(Paragraph::new(state_lines), chunks[4]);

    // Session Duration
    let session_line = Line::from(vec![
        Span::styled("Session: ", Style::default().fg(Color::DarkGray)),
        Span::styled(state.get_session_duration(), Style::default().fg(Color::Cyan).bold()),
    ]);
    frame.render_widget(Paragraph::new(session_line), chunks[6]);
}

/// Render game icon panel
fn render_game_panel(frame: &mut Frame, area: Rect, state: &TuiState) {
    let block = Block::default()
        .title(Span::styled(" Currently Playing ", Style::default().fg(Color::Cyan).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)))
        .padding(Padding::uniform(1));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    let content = if let Some(ref ascii) = state.ascii_art {
        // Display actual ASCII art
        let lines: Vec<Line> = ascii
            .lines()
            .map(|line| Line::from(Span::styled(line, Style::default().fg(Color::White))))
            .collect();
        Text::from(lines)
    } else if let Some(ref game) = state.current_game {
        // Show placeholder for current game
        create_game_placeholder(&game.game_id, &game.title, inner.width, inner.height)
    } else {
        // No game running - show waiting state
        create_waiting_placeholder(inner.width, inner.height)
    };

    let paragraph = Paragraph::new(content)
        .alignment(Alignment::Center);
    
    frame.render_widget(paragraph, inner);
}

/// Create a placeholder for a game
fn create_game_placeholder(game_id: &str, title: &str, _width: u16, height: u16) -> Text<'static> {
    let title_display = if title.is_empty() {
        "Unknown Game".to_string()
    } else if title.len() > 24 {
        format!("{}...", &title[..21])
    } else {
        title.to_string()
    };

    let id_display = game_id.to_string();
    
    // Calculate vertical centering
    let content_height = 11;
    let top_padding = if height > content_height { (height - content_height) / 2 } else { 0 };
    
    let mut lines = Vec::new();
    
    // Add top padding
    for _ in 0..top_padding {
        lines.push(Line::from(""));
    }
    
    // ASCII box art
    lines.push(Line::styled("    ╔═══════════════════════════╗", Style::default().fg(Color::Cyan)));
    lines.push(Line::styled("    ║                           ║", Style::default().fg(Color::Cyan)));
    lines.push(Line::styled("    ║    ┌───────────────┐      ║", Style::default().fg(Color::Cyan)));
    lines.push(Line::styled("    ║    │   PSP  GAME   │      ║", Style::default().fg(Color::Cyan)));
    lines.push(Line::styled("    ║    └───────────────┘      ║", Style::default().fg(Color::Cyan)));
    lines.push(Line::styled("    ║                           ║", Style::default().fg(Color::Cyan)));
    lines.push(Line::from(vec![
        Span::styled("    ║ ", Style::default().fg(Color::Cyan)),
        Span::styled(format!("{:^25}", title_display), Style::default().fg(Color::White).bold()),
        Span::styled(" ║", Style::default().fg(Color::Cyan)),
    ]));
    lines.push(Line::from(vec![
        Span::styled("    ║ ", Style::default().fg(Color::Cyan)),
        Span::styled(format!("{:^25}", id_display), Style::default().fg(Color::DarkGray)),
        Span::styled(" ║", Style::default().fg(Color::Cyan)),
    ]));
    lines.push(Line::styled("    ║                           ║", Style::default().fg(Color::Cyan)));
    lines.push(Line::styled("    ╚═══════════════════════════╝", Style::default().fg(Color::Cyan)));
    
    Text::from(lines)
}

/// Create a waiting placeholder
fn create_waiting_placeholder(_width: u16, height: u16) -> Text<'static> {
    let content_height = 5;
    let top_padding = if height > content_height { (height - content_height) / 2 } else { 0 };
    
    let mut lines = Vec::new();
    
    for _ in 0..top_padding {
        lines.push(Line::from(""));
    }
    
    lines.push(Line::styled("No game running", Style::default().fg(Color::DarkGray).italic()));
    
    Text::from(lines)
}

/// Render log messages
fn render_logs(frame: &mut Frame, area: Rect, state: &TuiState) {
    let block = Block::default()
        .title(Span::styled(" Activity Log ", Style::default().fg(Color::Cyan).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)))
        .padding(Padding::horizontal(1));

    let inner = block.inner(area);
    frame.render_widget(block, area);
    
    // Calculate how many log entries we can show
    let max_visible = inner.height.saturating_sub(1) as usize;

    let items: Vec<ListItem> = state
        .log_messages
        .iter()
        .rev()
        .take(max_visible.max(1))
        .rev()
        .map(|entry| {
            let (icon, level_color) = match entry.level {
                LogLevel::Info => ("•", Color::Blue),
                LogLevel::Warn => ("!", Color::Yellow),
                LogLevel::Error => ("✗", Color::Red),
                LogLevel::Success => ("✓", Color::Green),
            };

            let content = Line::from(vec![
                Span::styled(&entry.timestamp, Style::default().fg(Color::DarkGray)),
                Span::raw(" "),
                Span::styled(icon, Style::default().fg(level_color).bold()),
                Span::raw(" "),
                Span::styled(&entry.message, Style::default().fg(Color::Gray)),
            ]);
            ListItem::new(content)
        })
        .collect();

    let list = List::new(items);
    frame.render_widget(list, inner);
}

/// Render footer
fn render_footer(frame: &mut Frame, area: Rect, state: &TuiState) {
    let footer_content = Line::from(vec![
        Span::styled(" Press ", Style::default().fg(Color::DarkGray)),
        Span::styled("Q", Style::default().fg(Color::Yellow).bold()),
        Span::styled(" to quit ", Style::default().fg(Color::DarkGray)),
        Span::styled("│", Style::default().fg(Color::Rgb(50, 50, 70))),
        Span::styled(" ", Style::default()),
        Span::styled(&state.status_message, Style::default().fg(Color::Cyan)),
    ]);

    let footer = Paragraph::new(footer_content)
        .alignment(Alignment::Left);

    frame.render_widget(footer, area);
}
