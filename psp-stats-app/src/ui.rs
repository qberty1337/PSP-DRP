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
pub fn render_stats(frame: &mut Frame, stats: &StatsData, scroll_offset: usize, 
                    selector_mode: bool, selected_index: usize) {
    let area = frame.area();
    
    // Create outer border - highlight in selector mode
    let border_color = if selector_mode {
        Color::Rgb(100, 100, 200) // Brighter blue border in selector mode
    } else {
        Color::Rgb(60, 60, 80)
    };
    
    let outer_block = Block::default()
        .borders(Borders::ALL)
        .border_style(Style::default().fg(border_color))
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
    
    render_header(frame, chunks[0], stats, selector_mode);
    render_bar_graph(frame, chunks[1], stats, selector_mode);
    render_game_list(frame, chunks[2], stats, scroll_offset, selector_mode, selected_index);
    render_footer(frame, chunks[3], selector_mode);
}

/// Render stats header with summary info
fn render_header(frame: &mut Frame, area: Rect, stats: &StatsData, selector_mode: bool) {
    let visible_games: usize = if selector_mode {
        stats.games.len()
    } else {
        stats.games.iter().filter(|g| !g.hidden).count()
    };
    let hidden_count = stats.games.iter().filter(|g| g.hidden).count();
    
    let header_content = if selector_mode {
        Line::from(vec![
            Span::styled(" [SELECTOR MODE] ", Style::default().fg(Color::Black).bg(Color::Yellow).bold()),
            Span::styled("  ", Style::default()),
            Span::styled(format!("{} games", stats.games.len()), Style::default().fg(Color::Cyan)),
            Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{} hidden", hidden_count), Style::default().fg(Color::Red)),
        ])
    } else {
        Line::from(vec![
            Span::styled(" # ", Style::default().fg(Color::Magenta)),
            Span::styled("GAME STATISTICS", Style::default().fg(Color::White).bold()),
            Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{} games", visible_games), Style::default().fg(Color::Cyan)),
            Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
            Span::styled(format!("{} sessions", stats.total_sessions), Style::default().fg(Color::Yellow)),
            Span::styled("  |  ", Style::default().fg(Color::DarkGray)),
            Span::styled("Total: ", Style::default().fg(Color::DarkGray)),
            Span::styled(format_duration(stats.total_playtime), Style::default().fg(Color::Green).bold()),
        ])
    };

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
fn render_bar_graph(frame: &mut Frame, area: Rect, stats: &StatsData, selector_mode: bool) {
    let block = Block::default()
        .title(Span::styled(" Top Games ", Style::default().fg(Color::Yellow).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    // Filter games based on mode
    let visible_games: Vec<_> = if selector_mode {
        stats.games.iter().collect()
    } else {
        stats.games.iter().filter(|g| !g.hidden).collect()
    };

    if visible_games.is_empty() {
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
    let max_seconds = visible_games.iter()
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

    let lines: Vec<Line> = visible_games.iter()
        .take(inner.height as usize)
        .enumerate()
        .map(|(i, game)| {
            // Truncate title if needed
            let display_title = if game.title.chars().count() > title_width {
                let truncated: String = game.title.chars().take(title_width - 2).collect();
                format!("{}..", truncated)
            } else {
                format!("{:<width$}", game.title, width = title_width)
            };
            
            // Add hidden indicator
            let title = if game.hidden {
                format!("[H] {}", &display_title[..display_title.len().saturating_sub(4)])
            } else {
                display_title
            };

            // Calculate bar length
            let ratio = game.total_seconds as f64 / max_seconds;
            let bar_len = ((ratio * bar_area_width as f64) as usize).max(1);
            
            // Create bar
            let bar: String = "█".repeat(bar_len);
            
            // Format time
            let time = format_duration_short(game.total_seconds);

            let color = if game.hidden {
                Color::DarkGray  // Dim color for hidden games
            } else {
                bar_colors[i % bar_colors.len()]
            };
            
            Line::from(vec![
                Span::styled(title, Style::default().fg(if game.hidden { Color::DarkGray } else { Color::White })),
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
fn render_game_list(frame: &mut Frame, area: Rect, stats: &StatsData, scroll_offset: usize,
                    selector_mode: bool, selected_index: usize) {
    let title = if selector_mode {
        " Select Game (X to toggle hide) "
    } else {
        " All Games "
    };
    
    let block = Block::default()
        .title(Span::styled(title, Style::default().fg(Color::Cyan).bold()))
        .title_alignment(Alignment::Left)
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Rgb(50, 50, 70)));

    let inner = block.inner(area);
    frame.render_widget(block, area);

    // Filter games based on mode
    let visible_games: Vec<(usize, &crate::stats::GameStats)> = if selector_mode {
        stats.games.iter().enumerate().collect()
    } else {
        stats.games.iter().enumerate().filter(|(_, g)| !g.hidden).collect()
    };

    if visible_games.is_empty() {
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
    let hidden_width = if selector_mode { 4 } else { 0 };  // "[H] " in selector mode
    let fixed_width = rank_width + playtime_width + sessions_width + hidden_width + 4;
    let title_width = available_width.saturating_sub(fixed_width).max(10);

    // Create list items
    let items: Vec<ListItem> = visible_games
        .iter()
        .enumerate()
        .skip(scroll_offset)
        .take(inner.height as usize)
        .map(|(display_idx, (original_idx, game))| {
            let rank = display_idx + 1;
            
            // Check if this is the selected item
            let is_selected = selector_mode && *original_idx == selected_index;
            
            let rank_color = if is_selected {
                Color::White
            } else {
                match rank {
                    1 => Color::Yellow,     // Gold
                    2 => Color::Gray,       // Silver  
                    3 => Color::Rgb(205, 127, 50), // Bronze
                    _ => Color::DarkGray,
                }
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

            // Build content with optional hidden indicator and selection highlight
            let title_style = if is_selected {
                Style::default().fg(Color::Black).bg(Color::Yellow).bold()
            } else if game.hidden {
                Style::default().fg(Color::DarkGray)
            } else {
                Style::default().fg(Color::White).bold()
            };
            
            let hidden_indicator = if selector_mode && game.hidden {
                Span::styled("[H]", Style::default().fg(Color::Red))
            } else if selector_mode {
                Span::styled("   ", Style::default())
            } else {
                Span::styled("", Style::default())
            };

            let content = Line::from(vec![
                Span::styled(format!("{:>2}.", rank), Style::default().fg(rank_color)),
                hidden_indicator,
                Span::styled(" ", Style::default()),
                Span::styled(format!("{:<width$}", display_title, width = title_width), title_style),
                Span::styled(" ", Style::default()),
                Span::styled(format!("{:>10}", playtime), Style::default().fg(if game.hidden { Color::DarkGray } else { Color::Green })),
                Span::styled(" ", Style::default()),
                Span::styled(sessions, Style::default().fg(if game.hidden { Color::DarkGray } else { Color::Cyan })),
            ]);
            ListItem::new(content)
        })
        .collect();

    let list = List::new(items);
    frame.render_widget(list, inner);
}

/// Render footer with controls
fn render_footer(frame: &mut Frame, area: Rect, selector_mode: bool) {
    let version = env!("CARGO_PKG_VERSION");
    
    let footer_content = if selector_mode {
        Line::from(vec![
            Span::styled(" ", Style::default()),
            Span::styled("X", Style::default().fg(Color::Yellow).bold()),
            Span::styled(" toggle hide ", Style::default().fg(Color::DarkGray)),
            Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
            Span::styled(" ", Style::default()),
            Span::styled("UP/DOWN", Style::default().fg(Color::Yellow).bold()),
            Span::styled(" select ", Style::default().fg(Color::DarkGray)),
            Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
            Span::styled(" ", Style::default()),
            Span::styled("[]", Style::default().fg(Color::Yellow).bold()),
            Span::styled(" exit selector", Style::default().fg(Color::DarkGray)),
        ])
    } else {
        Line::from(vec![
            Span::styled(" ", Style::default()),
            Span::styled("[]", Style::default().fg(Color::Yellow).bold()),
            Span::styled(" selector ", Style::default().fg(Color::DarkGray)),
            Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
            Span::styled(" ", Style::default()),
            Span::styled("UP/DOWN", Style::default().fg(Color::Yellow).bold()),
            Span::styled(" scroll ", Style::default().fg(Color::DarkGray)),
            Span::styled("|", Style::default().fg(Color::Rgb(50, 50, 70))),
            Span::styled(format!(" PSP DRP v{}", version), Style::default().fg(Color::Cyan)),
        ])
    };

    let footer = Paragraph::new(footer_content)
        .alignment(Alignment::Left);

    frame.render_widget(footer, area);
}
