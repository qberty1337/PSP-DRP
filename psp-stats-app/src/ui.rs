//! UI rendering functions for the stats view
//!
//! Renders game statistics in a TUI similar to the desktop companion,
//! but simplified for PSP's 480x272 screen (approximately 60x17 characters
//! with default embedded-graphics font).

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use ratatui::{
    prelude::*,
    widgets::{Block, Borders, List, ListItem, Paragraph},
};

use crate::stats::{format_duration, format_duration_short, StatsData};

/// Main stats rendering function
pub fn render_stats(frame: &mut Frame, stats: &StatsData, scroll_offset: usize) {
    let area = frame.area();
    
    // Create outer border
    let outer_block = Block::default()
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(60, 60, 80)))
        .style(Style::default().bg(Color::Rgb(15, 15, 25)));
    
    frame.render_widget(outer_block.clone(), area);
    let inner_area = outer_block.inner(area);
    
    // Layout: Header, Bar Graph, Game List, Footer
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(2),  // Header
            Constraint::Length(5),  // Top games bar graph
            Constraint::Min(4),     // Game list
            Constraint::Length(1),  // Footer
        ])
        .split(inner_area);
    
    render_header(frame, chunks[0], stats);
    render_bar_graph(frame, chunks[1], stats);
    render_game_list(frame, chunks[2], stats, scroll_offset);
    render_footer(frame, chunks[3]);
}

/// Render stats header with summary info
fn render_header(frame: &mut Frame, area: Rect, stats: &StatsData) {
    let header_content = Line::from(vec![
        Span::styled(" # ", Style::default().fg(Color::Magenta)),
        Span::styled("GAME STATISTICS", Style::default().fg(Color::White).bold()),
        Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
        Span::styled(format!("{} games", stats.games.len()), Style::default().fg(Color::Cyan)),
        Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
        Span::styled(format!("{} sessions", stats.total_sessions), Style::default().fg(Color::Yellow)),
        Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
        Span::styled("Total: ", Style::default().fg(Color::DarkGray)),
        Span::styled(format_duration(stats.total_playtime), Style::default().fg(Color::Green).bold()),
    ]);

    let header = Paragraph::new(header_content)
        .block(
            Block::default()
                .borders(Borders::BOTTOM)
                .border_style(Style::default().fg(Color::Rgb(60, 60, 80)))
        )
        .alignment(Alignment::Center);

    frame.render_widget(header, area);
}

/// Render horizontal bar graph of top games
fn render_bar_graph(frame: &mut Frame, area: Rect, stats: &StatsData) {
    let block = Block::default()
        .title(Span::styled(" Top Games ", Style::default().fg(Color::Yellow).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    if stats.games.is_empty() {
        let empty_msg = Paragraph::new(Span::styled("No data", Style::default().fg(Color::DarkGray).italic()))
            .alignment(Alignment::Center);
        frame.render_widget(empty_msg, inner);
        return;
    }

    // Title width for display
    let title_width: usize = 20;
    
    // Calculate bar width based on available space
    let bar_area_width = inner.width.saturating_sub(title_width as u16 + 6) as usize;

    // Get max playtime for scaling
    let max_seconds = stats.games.iter()
        .map(|g| g.total_seconds)
        .max()
        .unwrap_or(1)
        .max(1) as f64;

    // Bar colors
    let bar_colors = [
        Color::Yellow,
        Color::Cyan,
        Color::Magenta,
        Color::Green,
        Color::Blue,
    ];

    let lines: Vec<Line> = stats.games.iter()
        .take(inner.height as usize)
        .enumerate()
        .map(|(i, game)| {
            // Truncate title if needed
            let title = if game.title.chars().count() > title_width {
                let truncated: String = game.title.chars().take(title_width - 2).collect();
                format!("{}..", truncated)
            } else {
                format!("{:<width$}", game.title, width = title_width)
            };

            // Calculate bar length
            let ratio = game.total_seconds as f64 / max_seconds;
            let bar_len = ((ratio * bar_area_width as f64) as usize).max(1);
            
            // Create bar
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

/// Render the scrollable game list
fn render_game_list(frame: &mut Frame, area: Rect, stats: &StatsData, scroll_offset: usize) {
    let block = Block::default()
        .title(Span::styled(" All Games ", Style::default().fg(Color::Cyan).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    if stats.games.is_empty() {
        let empty_msg = Paragraph::new(Text::from(vec![
            Line::from(""),
            Line::from(Span::styled("No games played yet!", Style::default().fg(Color::DarkGray).italic())),
        ]))
        .alignment(Alignment::Center);
        frame.render_widget(empty_msg, inner);
        return;
    }

    // Calculate column widths
    let available_width = inner.width as usize;
    let rank_width = 4;       // "#1. "
    let playtime_width = 10;  // "999h 59m"
    let sessions_width = 10;  // "(99 plays)"
    let fixed_width = rank_width + playtime_width + sessions_width + 4;
    let title_width = available_width.saturating_sub(fixed_width).max(10);

    // Create list items
    let items: Vec<ListItem> = stats.games
        .iter()
        .enumerate()
        .skip(scroll_offset)
        .take(inner.height as usize)
        .map(|(i, game)| {
            let rank = i + 1;
            let rank_color = match rank {
                1 => Color::Yellow,     // Gold
                2 => Color::Gray,       // Silver  
                3 => Color::Rgb(205, 127, 50), // Bronze
                _ => Color::DarkGray,
            };

            // Truncate title if needed
            let display_title = if game.title.len() > title_width {
                format!("{}...", &game.title[..title_width.saturating_sub(3)])
            } else {
                game.title.clone()
            };

            // Format playtime
            let playtime = format_duration(game.total_seconds);

            // Format session count
            let sessions = format!(
                "({} {})",
                game.session_count,
                if game.session_count == 1 { "play" } else { "plays" }
            );

            let content = Line::from(vec![
                Span::styled(format!("{:>2}.", rank), Style::default().fg(rank_color)),
                Span::styled(" ", Style::default()),
                Span::styled(format!("{:<width$}", display_title, width = title_width), Style::default().fg(Color::White).bold()),
                Span::styled(" ", Style::default()),
                Span::styled(format!("{:>10}", playtime), Style::default().fg(Color::Green)),
                Span::styled(" ", Style::default()),
                Span::styled(sessions, Style::default().fg(Color::Cyan)),
            ]);
            ListItem::new(content)
        })
        .collect();

    let list = List::new(items);
    frame.render_widget(list, inner);
}

/// Render footer with controls
fn render_footer(frame: &mut Frame, area: Rect) {
    let version = env!("CARGO_PKG_VERSION");
    let footer_content = Line::from(vec![
        Span::styled(" ", Style::default()),
        Span::styled("HOME", Style::default().fg(Color::Yellow).bold()),
        Span::styled(" to exit ", Style::default().fg(Color::DarkGray)),
        Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
        Span::styled(" ", Style::default()),
        Span::styled("UP/DOWN", Style::default().fg(Color::Yellow).bold()),
        Span::styled(" to scroll ", Style::default().fg(Color::DarkGray)),
        Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
        Span::styled(format!(" PSP DRP v{}", version), Style::default().fg(Color::Cyan)),
    ]);

    let footer = Paragraph::new(footer_content)
        .alignment(Alignment::Left);

    frame.render_widget(footer, area);
}
