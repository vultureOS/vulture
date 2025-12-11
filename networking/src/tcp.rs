//! # TCP Implementation
//!
//! Transmission Control Protocol state machine.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::Ipv4Addr;

/// TCP connection states
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TcpState {
    Closed,
    Listen,
    SynSent,
    SynReceived,
    Established,
    FinWait1,
    FinWait2,
    CloseWait,
    Closing,
    LastAck,
    TimeWait,
}

/// TCP header flags
#[derive(Debug, Clone, Copy)]
pub struct TcpFlags {
    pub fin: bool,
    pub syn: bool,
    pub rst: bool,
    pub psh: bool,
    pub ack: bool,
    pub urg: bool,
}

impl TcpFlags {
    pub fn from_byte(byte: u8) -> Self {
        Self {
            fin: byte & 0x01 != 0,
            syn: byte & 0x02 != 0,
            rst: byte & 0x04 != 0,
            psh: byte & 0x08 != 0,
            ack: byte & 0x10 != 0,
            urg: byte & 0x20 != 0,
        }
    }

    pub fn to_byte(&self) -> u8 {
        let mut b = 0u8;
        if self.fin {
            b |= 0x01;
        }
        if self.syn {
            b |= 0x02;
        }
        if self.rst {
            b |= 0x04;
        }
        if self.psh {
            b |= 0x08;
        }
        if self.ack {
            b |= 0x10;
        }
        if self.urg {
            b |= 0x20;
        }
        b
    }
}

/// TCP header
#[derive(Debug, Clone)]
pub struct TcpHeader {
    pub src_port: u16,
    pub dst_port: u16,
    pub seq_num: u32,
    pub ack_num: u32,
    pub data_offset: u8,
    pub flags: TcpFlags,
    pub window: u16,
    pub checksum: u16,
    pub urgent_ptr: u16,
}

impl TcpHeader {
    pub const MIN_SIZE: usize = 20;

    /// Parse a TCP header from raw bytes
    pub fn parse(data: &[u8]) -> Option<(Self, &[u8])> {
        if data.len() < Self::MIN_SIZE {
            return None;
        }

        let data_offset = data[12] >> 4;
        let header_len = (data_offset as usize) * 4;

        if data.len() < header_len {
            return None;
        }

        let header = Self {
            src_port: u16::from_be_bytes([data[0], data[1]]),
            dst_port: u16::from_be_bytes([data[2], data[3]]),
            seq_num: u32::from_be_bytes([data[4], data[5], data[6], data[7]]),
            ack_num: u32::from_be_bytes([data[8], data[9], data[10], data[11]]),
            data_offset,
            flags: TcpFlags::from_byte(data[13]),
            window: u16::from_be_bytes([data[14], data[15]]),
            checksum: u16::from_be_bytes([data[16], data[17]]),
            urgent_ptr: u16::from_be_bytes([data[18], data[19]]),
        };

        Some((header, &data[header_len..]))
    }
}

/// TCP connection state machine
pub struct TcpConnection {
    pub state: TcpState,
    pub local_addr: Ipv4Addr,
    pub local_port: u16,
    pub remote_addr: Ipv4Addr,
    pub remote_port: u16,
    pub send_seq: u32,
    pub recv_seq: u32,
    pub send_window: u16,
    pub recv_window: u16,
}

impl TcpConnection {
    /// Create a new TCP connection
    pub fn new(local_addr: Ipv4Addr, local_port: u16) -> Self {
        Self {
            state: TcpState::Closed,
            local_addr,
            local_port,
            remote_addr: Ipv4Addr::UNSPECIFIED,
            remote_port: 0,
            send_seq: 0,
            recv_seq: 0,
            send_window: 65535,
            recv_window: 65535,
        }
    }

    /// Initiate a connection (active open)
    pub fn connect(&mut self, remote_addr: Ipv4Addr, remote_port: u16) {
        self.remote_addr = remote_addr;
        self.remote_port = remote_port;
        self.state = TcpState::SynSent;
        // Send SYN
        self.send_seq = 1000; // Initial sequence number (should be random)
    }

    /// Listen for connections (passive open)
    pub fn listen(&mut self) {
        self.state = TcpState::Listen;
    }

    /// Handle an incoming segment
    pub fn handle_segment(&mut self, header: &TcpHeader, _data: &[u8]) {
        match self.state {
            TcpState::Listen => {
                if header.flags.syn {
                    self.remote_addr = Ipv4Addr::UNSPECIFIED; // would be from IP header
                    self.remote_port = header.src_port;
                    self.recv_seq = header.seq_num + 1;
                    self.state = TcpState::SynReceived;
                }
            }
            TcpState::SynSent => {
                if header.flags.syn && header.flags.ack {
                    self.recv_seq = header.seq_num + 1;
                    self.send_seq += 1;
                    self.state = TcpState::Established;
                }
            }
            TcpState::SynReceived => {
                if header.flags.ack {
                    self.state = TcpState::Established;
                }
            }
            TcpState::Established => {
                if header.flags.fin {
                    self.recv_seq = header.seq_num + 1;
                    self.state = TcpState::CloseWait;
                }
            }
            TcpState::FinWait1 => {
                if header.flags.ack {
                    self.state = TcpState::FinWait2;
                }
            }
            TcpState::FinWait2 => {
                if header.flags.fin {
                    self.recv_seq = header.seq_num + 1;
                    self.state = TcpState::TimeWait;
                }
            }
            TcpState::CloseWait => {
                // Application should call close
            }
            TcpState::LastAck => {
                if header.flags.ack {
                    self.state = TcpState::Closed;
                }
            }
            _ => {}
        }
    }

    /// Close the connection
    pub fn close(&mut self) {
        match self.state {
            TcpState::Established => {
                self.state = TcpState::FinWait1;
                // Send FIN
            }
            TcpState::CloseWait => {
                self.state = TcpState::LastAck;
                // Send FIN
            }
            _ => {}
        }
    }
}
