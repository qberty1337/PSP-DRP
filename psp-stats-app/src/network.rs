//! Network module for fetching stats from the desktop companion
//!
//! Uses PSP WiFi to connect to the desktop companion via UDP and
//! request usage statistics.
//!
//! Based on the working C plugin implementation in psp-plugin/net/src/network.c

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::c_void;

use psp::sys::{
    // Network init
    sceNetInit, sceNetTerm,
    sceNetInetInit, sceNetInetTerm,
    sceNetApctlInit, sceNetApctlTerm,
    sceNetApctlConnect, sceNetApctlGetState, sceNetApctlDisconnect,
    ApctlState,
    // Sockets
    sceNetInetSocket, sceNetInetClose,
    sceNetInetSendto, sceNetInetRecvfrom,
    sceNetInetSetsockopt,
    sockaddr, socklen_t,
    // Utility modules
    sceUtilityLoadNetModule, sceUtilityUnloadNetModule,
    NetModule,
    // Delay
    sceKernelDelayThread,
};

use crate::stats::{parse_usage_json, StatsData};

/// Protocol constants matching the desktop companion
const MAGIC: &[u8; 4] = b"PSPR";
const MSG_TYPE_STATS_REQUEST: u8 = 0x05;
const MSG_TYPE_STATS_RESPONSE: u8 = 0x12;

/// Known return codes (matching C plugin)
const NET_MODULE_ALREADY_LOADED: i32 = 0x80110F01_u32 as i32;
const NET_ALREADY_INITIALIZED: i32 = 0x80410003_u32 as i32;
const NET_LIBRARY_ALREADY_LOADED: i32 = 0x80110802_u32 as i32;

/// Network initialization state
pub struct NetworkManager {
    initialized: bool,
    socket: i32,
    companion_ip: [u8; 4],
    companion_port: u16,
}

/// sockaddr_in structure for PSP (IPv4)
#[repr(C)]
#[derive(Copy, Clone)]
struct SockAddrIn {
    sin_len: u8,
    sin_family: u8,
    sin_port: u16,
    sin_addr: u32,
    sin_zero: [u8; 8],
}

impl NetworkManager {
    /// Create a new network manager
    pub fn new() -> Self {
        Self {
            initialized: false,
            socket: -1,
            companion_ip: [0; 4],
            companion_port: 9276,  // Default port
        }
    }
    
    /// Set the companion IP address
    pub fn set_companion_ip(&mut self, ip: [u8; 4]) {
        self.companion_ip = ip;
    }
    
    /// Set the companion port
    pub fn set_companion_port(&mut self, port: u16) {
        self.companion_port = port;
    }
    
    /// Initialize network stack (matching C plugin approach)
    /// Returns true if successful, false otherwise
    pub fn init(&mut self) -> bool {
        if self.initialized {
            return true;
        }
        
        unsafe {
            // Check if already connected (like C plugin does)
            let mut state: i32 = 0;
            let ret = sceNetApctlGetState(&mut state as *mut i32 as *mut ApctlState);
            if ret == 0 {
                // Already initialized by game/system
                self.initialized = true;
                return true;
            }
            
            // Load required modules (like C plugin does)
            let ret = sceUtilityLoadNetModule(NetModule::NetCommon);
            if ret < 0 && ret != NET_MODULE_ALREADY_LOADED && ret != NET_LIBRARY_ALREADY_LOADED {
                return false;
            }
            
            let ret = sceUtilityLoadNetModule(NetModule::NetInet);
            if ret < 0 && ret != NET_MODULE_ALREADY_LOADED && ret != NET_LIBRARY_ALREADY_LOADED {
                return false;
            }
            
            // Initialize networking (matching C plugin parameters)
            // sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024)
            let ret = sceNetInit(
                128 * 1024,  // 128KB pool
                42,          // Callout priority
                4 * 1024,    // Callout stack
                42,          // Netintr priority
                4 * 1024,    // Netintr stack
            );
            if ret < 0 && ret != NET_ALREADY_INITIALIZED {
                return false;
            }
            
            // Initialize inet library
            let ret = sceNetInetInit();
            if ret < 0 && ret != NET_ALREADY_INITIALIZED {
                sceNetTerm();
                return false;
            }
            
            // Initialize apctl (matching C plugin: 0x8000, 48)
            let ret = sceNetApctlInit(0x8000, 48);
            if ret < 0 && ret != NET_ALREADY_INITIALIZED {
                sceNetInetTerm();
                sceNetTerm();
                return false;
            }
            
            self.initialized = true;
        }
        
        true
    }
    
    /// Connect to WiFi using saved connection profile
    /// Matches the C plugin's connect_to_ap + wait_for_connection pattern
    /// Returns true if connected successfully
    pub fn connect_wifi(&self, connection_index: i32) -> bool {
        if !self.initialized {
            return false;
        }
        
        unsafe {
            // Check current network state (like C plugin)
            let mut state: i32 = 0;
            let ret = sceNetApctlGetState(&mut state as *mut i32 as *mut ApctlState);
            
            if ret == 0 {
                // Already have an IP - fully connected, reuse this connection
                if state == 4 {  // PSP_NET_APCTL_STATE_GOT_IP
                    return true;
                }
                
                // Game is connecting (state > 0), wait for it
                if state > 0 {
                    return self.wait_for_connection(30);
                }
            }
            
            // Only disconnect and reconnect if truly disconnected
            // Pre-disconnect to ensure clean state
            sceNetApctlDisconnect();
            sceKernelDelayThread(500_000);  // 500ms delay like C plugin
            
            // Connect to saved network profile
            let ret = sceNetApctlConnect(connection_index);
            if ret < 0 {
                return false;
            }
            
            // Wait for connection
            self.wait_for_connection(30)
        }
    }
    
    /// Wait for network connection (matching C plugin)
    /// timeout_seconds: how long to wait
    fn wait_for_connection(&self, timeout_seconds: i32) -> bool {
        unsafe {
            let mut timeout = timeout_seconds * 4;  // 250ms intervals (close to C's 300ms)
            
            while timeout > 0 {
                let mut state: i32 = 0;
                if sceNetApctlGetState(&mut state as *mut i32 as *mut ApctlState) < 0 {
                    return false;
                }
                
                // PSP_NET_APCTL_STATE_GOT_IP = 4
                if state == 4 {
                    return true;
                }
                
                // Disconnected (0) - failed
                if state == 0 && timeout < timeout_seconds * 4 - 2 {
                    // Only fail after a couple iterations to avoid false negatives
                    return false;
                }
                
                sceKernelDelayThread(250_000);  // 250ms
                timeout -= 1;
            }
            
            // Timeout - disconnect
            sceNetApctlDisconnect();
            false
        }
    }
    
    /// Create UDP socket
    pub fn create_socket(&mut self) -> bool {
        if !self.initialized {
            return false;
        }
        
        unsafe {
            // Create UDP socket (AF_INET=2, SOCK_DGRAM=2, protocol=0)
            let sock = sceNetInetSocket(2, 2, 0);
            if sock < 0 {
                return false;
            }
            
            // Set receive timeout (1 second)
            let timeout: i32 = 1_000_000;  // microseconds
            let ret = sceNetInetSetsockopt(
                sock,
                0xFFFF,  // SOL_SOCKET
                0x1006,  // SO_RCVTIMEO
                &timeout as *const i32 as *const c_void,
                4,
            );
            if ret < 0 {
                sceNetInetClose(sock);
                return false;
            }
            
            self.socket = sock;
        }
        
        true
    }
    
    /// Send stats request to companion
    pub fn send_stats_request(&self) -> bool {
        if self.socket < 0 {
            return false;
        }
        
        // Build stats request packet
        // Format: [MAGIC:4][MSG_TYPE:1]
        let mut packet = [0u8; 5];
        packet[0..4].copy_from_slice(MAGIC);
        packet[4] = MSG_TYPE_STATS_REQUEST;
        
        // Build destination address
        let addr = SockAddrIn {
            sin_len: 16,
            sin_family: 2,  // AF_INET
            sin_port: self.companion_port.to_be(),  // Network byte order
            sin_addr: u32::from_le_bytes(self.companion_ip),  // Little endian IP
            sin_zero: [0; 8],
        };
        
        unsafe {
            let sent = sceNetInetSendto(
                self.socket,
                packet.as_ptr() as *const c_void,
                packet.len(),
                0,
                &addr as *const SockAddrIn as *const sockaddr,
                16,
            );
            
            sent > 0
        }
    }
    
    /// Receive stats response from companion
    /// Returns the JSON data if successful
    pub fn receive_stats_response(&self) -> Option<Vec<u8>> {
        if self.socket < 0 {
            return None;
        }
        
        let mut buffer = [0u8; 2048];
        let mut json_data = Vec::new();
        let mut expected_chunks: u16 = 0;
        let mut received_chunks: u16 = 0;
        
        // Try to receive chunks (with timeout handled by socket)
        for _ in 0..10 {  // Max 10 chunks
            let mut from_addr: sockaddr = unsafe { core::mem::zeroed() };
            let mut from_len: socklen_t = 16;
            
            let received = unsafe {
                sceNetInetRecvfrom(
                    self.socket,
                    buffer.as_mut_ptr() as *mut c_void,
                    buffer.len(),
                    0,
                    &mut from_addr,
                    &mut from_len,
                )
            };
            
            if received < 5 {
                // Timeout or error
                if received_chunks > 0 && received_chunks >= expected_chunks {
                    break;  // We got all chunks
                }
                continue;
            }
            
            let data = &buffer[..received as usize];
            
            // Verify magic and message type
            if &data[0..4] != MAGIC || data[4] != MSG_TYPE_STATS_RESPONSE {
                continue;
            }
            
            // Parse stats response header
            // Format: [MAGIC:4][TYPE:1][total_games:2][total_playtime:8][chunk_idx:2][total_chunks:2][data_len:2][json...]
            if data.len() < 21 {
                continue;
            }
            
            let chunk_idx = u16::from_le_bytes([data[15], data[16]]);
            let total_chunks = u16::from_le_bytes([data[17], data[18]]);
            let data_len = u16::from_le_bytes([data[19], data[20]]) as usize;
            
            if expected_chunks == 0 {
                expected_chunks = total_chunks;
            }
            
            // Extract JSON chunk
            if data.len() >= 21 + data_len {
                json_data.extend_from_slice(&data[21..21 + data_len]);
                received_chunks += 1;
            }
            
            if received_chunks >= expected_chunks {
                break;
            }
        }
        
        if json_data.is_empty() {
            None
        } else {
            Some(json_data)
        }
    }
    
    /// Fetch stats from companion
    /// Full flow: send request, receive response, parse JSON
    pub fn fetch_stats(&self) -> Option<StatsData> {
        // Send request
        if !self.send_stats_request() {
            return None;
        }
        
        // Small delay for response
        unsafe {
            sceKernelDelayThread(50_000);  // 50ms
        }
        
        // Receive response
        let json_data = self.receive_stats_response()?;
        
        // Parse JSON
        let json_str = core::str::from_utf8(&json_data).ok()?;
        Some(parse_usage_json(json_str))
    }
    
    /// Fetch stats from companion, returning both parsed stats and raw JSON bytes
    /// Used for caching the response to a file
    pub fn fetch_stats_raw(&self) -> Option<(StatsData, Vec<u8>)> {
        // Send request
        if !self.send_stats_request() {
            return None;
        }
        
        // Small delay for response
        unsafe {
            sceKernelDelayThread(50_000);  // 50ms
        }
        
        // Receive response
        let json_data = self.receive_stats_response()?;
        
        // Parse JSON
        let json_str = core::str::from_utf8(&json_data).ok()?;
        let stats = parse_usage_json(json_str);
        
        Some((stats, json_data))
    }
    
    /// Cleanup
    pub fn cleanup(&mut self) {
        unsafe {
            if self.socket >= 0 {
                sceNetInetClose(self.socket);
                self.socket = -1;
            }
            
            if self.initialized {
                sceNetApctlDisconnect();
                sceNetApctlTerm();
                sceNetInetTerm();
                sceNetTerm();
                sceUtilityUnloadNetModule(NetModule::NetInet);
                sceUtilityUnloadNetModule(NetModule::NetCommon);
                self.initialized = false;
            }
        }
    }
}

impl Drop for NetworkManager {
    fn drop(&mut self) {
        self.cleanup();
    }
}
