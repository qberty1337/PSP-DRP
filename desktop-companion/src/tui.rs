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
use chrono::Datelike;

use crate::protocol::{GameInfo, PspState};

/// Maximum number of log messages to keep
const MAX_LOG_MESSAGES: usize = 50;

/// TUI event types
#[derive(Debug, Clone)]
pub enum TuiEvent {
    /// User wants to quit
    Quit,
    /// User wants to view stats
    ShowStats,
    /// User wants to hide stats
    HideStats,
}

/// Which view is currently active
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ViewMode {
    #[default]
    Main,
    Stats,
}

/// Game stats for display in stats view
#[derive(Debug, Clone)]
pub struct GameStats {
    pub title: String,
    #[allow(dead_code)]  // Reserved for future use (filtering, details view)
    pub game_id: String,
    pub total_seconds: u64,
    pub session_count: u32,
    pub last_played: String,
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
    /// Current view mode
    pub view_mode: ViewMode,
    /// All game stats for the stats view
    pub all_game_stats: Vec<GameStats>,
    /// Scroll offset for stats view
    pub stats_scroll: usize,
    /// All dates when games were played, mapping date (YYYY-MM-DD) to game titles
    pub play_dates: std::collections::HashMap<String, Vec<String>>,
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
    pub fn handle_events(&mut self, timeout: Duration, view_mode: ViewMode) -> io::Result<Option<TuiEvent>> {
        if event::poll(timeout)? {
            if let Event::Key(key) = event::read()? {
                if key.kind == KeyEventKind::Press {
                    match key.code {
                        KeyCode::Char('q') | KeyCode::Char('Q') => {
                            return Ok(Some(TuiEvent::Quit));
                        }
                        KeyCode::Esc => {
                            // Esc quits from main view, goes back from stats view
                            if view_mode == ViewMode::Stats {
                                return Ok(Some(TuiEvent::HideStats));
                            } else {
                                return Ok(Some(TuiEvent::Quit));
                            }
                        }
                        KeyCode::Char('s') | KeyCode::Char('S') => {
                            if view_mode == ViewMode::Main {
                                return Ok(Some(TuiEvent::ShowStats));
                            } else {
                                return Ok(Some(TuiEvent::HideStats));
                            }
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
    match state.view_mode {
        ViewMode::Main => render_main_view(frame, state),
        ViewMode::Stats => render_stats_view(frame, state),
    }
}

/// Render the main monitoring view
fn render_main_view(frame: &mut Frame, state: &TuiState) {
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
        Span::styled(" DRP ", Style::default().fg(Color::Magenta).bold()),
        Span::styled(
            format!("v{}", env!("CARGO_PKG_VERSION")),
            Style::default().fg(Color::DarkGray)
        ),
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
        Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
        Span::styled(" ", Style::default()),
        Span::styled("S", Style::default().fg(Color::Yellow).bold()),
        Span::styled(" for stats ", Style::default().fg(Color::DarkGray)),
        Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
        Span::styled(" ", Style::default()),
        Span::styled(&state.status_message, Style::default().fg(Color::Cyan)),
    ]);

    let footer = Paragraph::new(footer_content)
        .alignment(Alignment::Left);

    frame.render_widget(footer, area);
}

/// Render the stats view showing all games played
fn render_stats_view(frame: &mut Frame, state: &TuiState) {
    let area = frame.area();

    // Create outer border for the entire app
    let outer_block = Block::default()
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(60, 60, 80)))
        .style(Style::default().bg(Color::Rgb(15, 15, 25)));
    
    frame.render_widget(outer_block.clone(), area);
    let inner_area = outer_block.inner(area);

    // Stats view layout
    let main_chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),   // Header/Title
            Constraint::Length(10),  // Visualization (bar graph + calendar)
            Constraint::Min(6),      // Stats list
            Constraint::Length(1),   // Footer
        ])
        .split(inner_area);

    render_stats_header(frame, main_chunks[0], state);
    render_stats_visualization(frame, main_chunks[1], state);
    render_stats_list(frame, main_chunks[2], state);
    render_stats_footer(frame, main_chunks[3]);
}

/// Render stats view header
fn render_stats_header(frame: &mut Frame, area: Rect, state: &TuiState) {
    let total_playtime: u64 = state.all_game_stats.iter().map(|g| g.total_seconds).sum();
    let total_sessions: u32 = state.all_game_stats.iter().map(|g| g.session_count).sum();
    
    let header_content = Line::from(vec![
        Span::styled(" # ", Style::default().fg(Color::Magenta)),
        Span::styled("GAME STATISTICS", Style::default().fg(Color::White).bold()),
        Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
        Span::styled(format!("{} games", state.all_game_stats.len()), Style::default().fg(Color::Cyan)),
        Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
        Span::styled(format!("{} sessions", total_sessions), Style::default().fg(Color::Yellow)),
        Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
        Span::styled("Total: ", Style::default().fg(Color::DarkGray)),
        Span::styled(format_duration_long(total_playtime), Style::default().fg(Color::Green).bold()),
    ]);

    let header = Paragraph::new(header_content)
        .block(
            Block::default()
                .borders(Borders::BOTTOM)
                .border_style(Style::default().fg(Color::Rgb(60, 60, 80)))
                .padding(Padding::horizontal(1))
        )
        .alignment(Alignment::Center);

    frame.render_widget(header, area);
}

/// Render visualization section with bar graph and calendar
fn render_stats_visualization(frame: &mut Frame, area: Rect, state: &TuiState) {
    // Split into bar graph (left) and calendar (right)
    let viz_chunks = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage(55),  // Bar graph
            Constraint::Percentage(45),  // Calendar
        ])
        .split(area);

    render_bar_graph(frame, viz_chunks[0], state);
    render_calendar(frame, viz_chunks[1], state);
}

/// Render horizontal bar graph of top games
fn render_bar_graph(frame: &mut Frame, area: Rect, state: &TuiState) {
    let block = Block::default()
        .title(Span::styled(" Top Games ", Style::default().fg(Color::Yellow).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)))
        .padding(Padding::new(1, 1, 0, 0));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    if state.all_game_stats.is_empty() {
        let empty_msg = Paragraph::new(Span::styled("No data", Style::default().fg(Color::DarkGray).italic()))
            .alignment(Alignment::Center);
        frame.render_widget(empty_msg, inner);
        return;
    }

    // Title width - show more of the game name
    let title_width: usize = 28;
    
    // Calculate bar width based on available space (reserve space for title + time + spacing)
    let bar_area_width = inner.width.saturating_sub(title_width as u16 + 8) as usize;

    // Get max playtime for scaling
    let max_seconds = state.all_game_stats.iter()
        .map(|g| g.total_seconds)
        .max()
        .unwrap_or(1)
        .max(1) as f64;

    // Create bar lines for top games
    let bar_colors = [
        Color::Yellow,
        Color::Cyan,
        Color::Magenta,
        Color::Green,
        Color::Blue,
    ];

    let lines: Vec<Line> = state.all_game_stats.iter()
        .take(inner.height as usize)
        .enumerate()
        .map(|(i, game)| {
            // Truncate title with ellipsis
            let title = if game.title.chars().count() > title_width {
                let truncated: String = game.title.chars().take(title_width - 3).collect();
                format!("{}...", truncated)
            } else {
                format!("{:<width$}", game.title, width = title_width)
            };

            // Calculate bar length - linear scaling with minimum of 1
            let ratio = game.total_seconds as f64 / max_seconds;
            let bar_len = ((ratio * bar_area_width as f64) as usize).max(1);
            
            // Create bar using block characters
            let bar: String = "█".repeat(bar_len);
            
            // Format time
            let time = format_duration_short(game.total_seconds);

            let color = bar_colors[i % bar_colors.len()];
            
            Line::from(vec![
                Span::styled(title, Style::default().fg(Color::White)),
                Span::styled(" ", Style::default()),
                Span::styled(bar, Style::default().fg(color)),
                Span::styled(format!(" {}", time), Style::default().fg(Color::DarkGray)),
            ])
        })
        .collect();

    let paragraph = Paragraph::new(lines);
    frame.render_widget(paragraph, inner);
}

/// Render calendar heatmap showing past 3 months
fn render_calendar(frame: &mut Frame, area: Rect, state: &TuiState) {
    let block = Block::default()
        .title(Span::styled(" Activity ", Style::default().fg(Color::Cyan).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    let now = chrono::Local::now();
    let current_year = now.year();
    let current_month = now.month();
    let current_day = now.day();

    // Generate data for 3 months (2 months ago, last month, current)
    let months: Vec<(i32, u32, bool)> = (0..3).rev().map(|months_ago| {
        let (y, m) = subtract_months(current_year, current_month, months_ago);
        let is_current = months_ago == 0;
        (y, m, is_current)
    }).collect();

    let mut lines: Vec<Line> = Vec::new();
    
    // Header row with month names (each month takes 21 chars: 7 days * 3 chars each)
    let mut header_spans: Vec<Span> = Vec::new();
    for (i, (year, month, _)) in months.iter().enumerate() {
        let month_name = get_short_month_name(*month);
        // Center month name in 21 char width
        let header = format!("{} '{}", month_name, year % 100);
        header_spans.push(Span::styled(format!("{:^21}", header), Style::default().fg(Color::White).bold()));
        if i < 2 {
            header_spans.push(Span::styled(" ", Style::default()));
        }
    }
    lines.push(Line::from(header_spans));
    
    // Day-of-week headers
    let mut dow_spans: Vec<Span> = Vec::new();
    for i in 0..3 {
        dow_spans.push(Span::styled("Mo ", Style::default().fg(Color::DarkGray)));
        dow_spans.push(Span::styled("Tu ", Style::default().fg(Color::DarkGray)));
        dow_spans.push(Span::styled("We ", Style::default().fg(Color::DarkGray)));
        dow_spans.push(Span::styled("Th ", Style::default().fg(Color::DarkGray)));
        dow_spans.push(Span::styled("Fr ", Style::default().fg(Color::DarkGray)));
        dow_spans.push(Span::styled("Sa ", Style::default().fg(Color::DarkGray)));
        dow_spans.push(Span::styled("Su", Style::default().fg(Color::DarkGray)));
        if i < 2 {
            dow_spans.push(Span::styled("  ", Style::default()));
        }
    }
    lines.push(Line::from(dow_spans));
    
    // Bar graph colors - same order as bar graph
    let game_colors = [
        Color::Yellow,
        Color::Cyan,
        Color::Magenta,
        Color::Green,
        Color::Blue,
    ];
    
    // Build a map of game title to color index based on position in all_game_stats
    let game_color_map: std::collections::HashMap<String, usize> = state.all_game_stats.iter()
        .enumerate()
        .map(|(i, g)| (g.title.clone(), i))
        .collect();
    
    // Generate a grid for each week row
    for week in 0..6 {
        let mut row_spans: Vec<Span> = Vec::new();
        
        for (month_idx, (year, month, is_current)) in months.iter().enumerate() {
            let first_day = chrono::NaiveDate::from_ymd_opt(*year, *month, 1).unwrap();
            let days_in_month = get_days_in_month(*year, *month);
            let first_weekday = first_day.weekday().num_days_from_monday() as i32;
            
            // Render 7 days for this week
            for weekday in 0..7i32 {
                let day_num = week as i32 * 7 + weekday - first_weekday + 1;
                
                if day_num >= 1 && day_num <= days_in_month as i32 {
                    let day = day_num as u32;
                    let date_str = format!("{:04}-{:02}-{:02}", year, month, day);
                    let is_today = *is_current && day == current_day;
                    
                    // Check if any games were played this day and get the color
                    let day_color = if let Some(games) = state.play_dates.get(&date_str) {
                        // Find the game with the lowest index (most played overall)
                        games.iter()
                            .filter_map(|title| game_color_map.get(title).copied())
                            .min()
                            .map(|idx| game_colors[idx % game_colors.len()])
                    } else {
                        None
                    };
                    
                    let style = if let Some(color) = day_color {
                        if is_today {
                            Style::default().fg(Color::Black).bg(color).bold()
                        } else {
                            Style::default().fg(color).bold()
                        }
                    } else if is_today {
                        Style::default().fg(Color::Black).bg(Color::White).bold()
                    } else {
                        Style::default().fg(Color::Rgb(70, 70, 90))
                    };
                    
                    // Format day with trailing space (except last in month section)
                    if weekday < 6 {
                        row_spans.push(Span::styled(format!("{:2} ", day), style));
                    } else {
                        row_spans.push(Span::styled(format!("{:2}", day), style));
                    }
                } else {
                    // Empty cell
                    if weekday < 6 {
                        row_spans.push(Span::styled("   ", Style::default()));
                    } else {
                        row_spans.push(Span::styled("  ", Style::default()));
                    }
                }
            }
            
            // Spacing between months
            if month_idx < 2 {
                row_spans.push(Span::styled("  ", Style::default()));
            }
        }
        
        lines.push(Line::from(row_spans));
    }

    let paragraph = Paragraph::new(lines).alignment(Alignment::Center);
    frame.render_widget(paragraph, inner);
}

/// Subtract months from a year/month, handling year rollover
fn subtract_months(year: i32, month: u32, months_ago: u32) -> (i32, u32) {
    let total_months = year * 12 + (month as i32 - 1) - months_ago as i32;
    let new_year = total_months / 12;
    let new_month = (total_months % 12 + 1) as u32;
    (new_year, new_month)
}

/// Get short month name
fn get_short_month_name(month: u32) -> &'static str {
    match month {
        1 => "Jan", 2 => "Feb", 3 => "Mar", 4 => "Apr",
        5 => "May", 6 => "Jun", 7 => "Jul", 8 => "Aug",
        9 => "Sep", 10 => "Oct", 11 => "Nov", 12 => "Dec",
        _ => "???",
    }
}

/// Get number of days in a month
fn get_days_in_month(year: i32, month: u32) -> u32 {
    let first_day = chrono::NaiveDate::from_ymd_opt(year, month, 1).unwrap();
    let next_month = if month == 12 {
        chrono::NaiveDate::from_ymd_opt(year + 1, 1, 1)
    } else {
        chrono::NaiveDate::from_ymd_opt(year, month + 1, 1)
    }.unwrap();
    next_month.signed_duration_since(first_day).num_days() as u32
}

/// Format duration as short string (e.g., "12h" or "45m")
fn format_duration_short(total_secs: u64) -> String {
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;

    if hours > 0 {
        format!("{}h", hours)
    } else if mins > 0 {
        format!("{}m", mins)
    } else {
        "<1m".to_string()
    }
}

/// Render the stats list
fn render_stats_list(frame: &mut Frame, area: Rect, state: &TuiState) {
    let block = Block::default()
        .title(Span::styled(" All Games (sorted by playtime) ", Style::default().fg(Color::Cyan).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)))
        .padding(Padding::new(1, 1, 0, 0));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    if state.all_game_stats.is_empty() {
        let empty_msg = Paragraph::new(Text::from(vec![
            Line::from(""),
            Line::from(Span::styled("No games played yet!", Style::default().fg(Color::DarkGray).italic())),
            Line::from(""),
            Line::from(Span::styled("Start playing a game on your PSP to track stats.", Style::default().fg(Color::DarkGray))),
        ]))
        .alignment(Alignment::Center);
        frame.render_widget(empty_msg, inner);
        return;
    }

    // Calculate column widths based on available space
    let available_width = inner.width as usize;
    let rank_width = 6;       // "#1.  " with medal
    let playtime_width = 12;  // "999h 59m 59s"
    let sessions_width = 12;  // "(999 plays)"
    let last_played_width = 12; // "MM-DD HH:MM"
    let fixed_width = rank_width + playtime_width + sessions_width + last_played_width + 6; // separators
    let title_width = available_width.saturating_sub(fixed_width).max(20);

    // Create list items
    let items: Vec<ListItem> = state.all_game_stats
        .iter()
        .enumerate()
        .map(|(i, game)| {
            let rank = i + 1;
            let rank_color = match rank {
                1 => Color::Yellow,     // Gold
                2 => Color::Gray,       // Silver  
                3 => Color::Rgb(205, 127, 50), // Bronze
                _ => Color::DarkGray,
            };
            let rank_prefix = match rank {
                1 => "[1]",
                2 => "[2]",
                3 => "[3]",
                _ => "   ",
            };

            // Truncate title if needed
            let display_title = if game.title.len() > title_width {
                format!("{}...", &game.title[..title_width.saturating_sub(3)])
            } else {
                game.title.clone()
            };

            // Format playtime
            let playtime = format_duration_long(game.total_seconds);

            // Format session count
            let sessions = format!(
                "({} {})",
                game.session_count,
                if game.session_count == 1 { "play" } else { "plays" }
            );

            // Format last played (extract just date/time part)
            let last_played = if game.last_played.len() >= 16 {
                // Format: "YYYY-MM-DD HH:MM" -> "MM-DD HH:MM"
                game.last_played[5..16].to_string()
            } else {
                game.last_played.clone()
            };

            let content = Line::from(vec![
                Span::styled(rank_prefix, Style::default().fg(rank_color).bold()),
                Span::styled(format!("{:>2}. ", rank), Style::default().fg(rank_color)),
                Span::styled(format!("{:<width$}", display_title, width = title_width), Style::default().fg(Color::White).bold()),
                Span::styled("  ", Style::default()),
                Span::styled(format!("{:>12}", playtime), Style::default().fg(Color::Green)),
                Span::styled("  ", Style::default()),
                Span::styled(format!("{:<12}", sessions), Style::default().fg(Color::Cyan)),
                Span::styled(format!("{:>11}", last_played), Style::default().fg(Color::DarkGray)),
            ]);
            ListItem::new(content)
        })
        .collect();

    let list = List::new(items);
    frame.render_widget(list, inner);
}

/// Render stats view footer
fn render_stats_footer(frame: &mut Frame, area: Rect) {
    let footer_content = Line::from(vec![
        Span::styled(" Press ", Style::default().fg(Color::DarkGray)),
        Span::styled("S", Style::default().fg(Color::Yellow).bold()),
        Span::styled(" or ", Style::default().fg(Color::DarkGray)),
        Span::styled("Esc", Style::default().fg(Color::Yellow).bold()),
        Span::styled(" to go back ", Style::default().fg(Color::DarkGray)),
        Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
        Span::styled(" ", Style::default()),
        Span::styled("Q", Style::default().fg(Color::Yellow).bold()),
        Span::styled(" to quit", Style::default().fg(Color::DarkGray)),
    ]);

    let footer = Paragraph::new(footer_content)
        .alignment(Alignment::Left);

    frame.render_widget(footer, area);
}

/// Format seconds as duration string (e.g., "12h 34m 56s")
fn format_duration_long(total_secs: u64) -> String {
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;
    let secs = total_secs % 60;

    if hours > 0 {
        format!("{}h {}m {}s", hours, mins, secs)
    } else if mins > 0 {
        format!("{}m {}s", mins, secs)
    } else {
        format!("{}s", secs)
    }
}
