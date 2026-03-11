//! # IPC Channels
//!
//! Bidirectional IPC channels using ring buffers for zero-copy
//! message passing between processes.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use spin::Mutex;

/// Maximum message size
pub const MAX_MSG_SIZE: usize = 4096;

/// Channel buffer capacity (number of messages)
const CHANNEL_CAPACITY: usize = 16;

/// A message in the channel
#[derive(Clone)]
pub struct Message {
    /// Message data
    pub data: [u8; MAX_MSG_SIZE],
    /// Actual data length
    pub len: usize,
    /// Sender process ID
    pub sender_pid: u64,
    /// Message type/tag
    pub msg_type: u32,
}

impl Message {
    pub const fn empty() -> Self {
        Self {
            data: [0; MAX_MSG_SIZE],
            len: 0,
            sender_pid: 0,
            msg_type: 0,
        }
    }

    pub fn new(data: &[u8], sender_pid: u64, msg_type: u32) -> Self {
        let mut msg = Self::empty();
        let copy_len = data.len().min(MAX_MSG_SIZE);
        msg.data[..copy_len].copy_from_slice(&data[..copy_len]);
        msg.len = copy_len;
        msg.sender_pid = sender_pid;
        msg.msg_type = msg_type;
        msg
    }

    /// Get the message data as a slice
    pub fn as_bytes(&self) -> &[u8] {
        &self.data[..self.len]
    }
}

/// Ring buffer for channel messages
pub struct RingBuffer {
    buffer: [Message; CHANNEL_CAPACITY],
    read_pos: usize,
    write_pos: usize,
    count: usize,
}

impl RingBuffer {
    pub const fn new() -> Self {
        Self {
            buffer: [const { Message::empty() }; CHANNEL_CAPACITY],
            read_pos: 0,
            write_pos: 0,
            count: 0,
        }
    }

    /// Push a message into the ring buffer
    pub fn push(&mut self, msg: Message) -> bool {
        if self.count >= CHANNEL_CAPACITY {
            return false; // Buffer full
        }
        self.buffer[self.write_pos] = msg;
        self.write_pos = (self.write_pos + 1) % CHANNEL_CAPACITY;
        self.count += 1;
        true
    }

    /// Pop a message from the ring buffer
    pub fn pop(&mut self) -> Option<Message> {
        if self.count == 0 {
            return None;
        }
        let msg = self.buffer[self.read_pos].clone();
        self.read_pos = (self.read_pos + 1) % CHANNEL_CAPACITY;
        self.count -= 1;
        Some(msg)
    }

    /// Check if the buffer is empty
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Check if the buffer is full
    pub fn is_full(&self) -> bool {
        self.count >= CHANNEL_CAPACITY
    }

    /// Get the number of pending messages
    pub fn pending(&self) -> usize {
        self.count
    }
}

/// A bidirectional IPC channel
pub struct Channel {
    /// Channel ID
    pub id: u64,
    /// Endpoint A → B buffer
    pub a_to_b: Mutex<RingBuffer>,
    /// Endpoint B → A buffer
    pub b_to_a: Mutex<RingBuffer>,
    /// PID of endpoint A
    pub pid_a: u64,
    /// PID of endpoint B
    pub pid_b: u64,
}

/// Channel endpoint (handle given to a process)
pub struct ChannelEndpoint {
    pub channel_id: u64,
    pub is_side_a: bool,
}

impl ChannelEndpoint {
    /// Send a message through this endpoint
    pub fn send(&self, channel: &Channel, msg: Message) -> bool {
        if self.is_side_a {
            channel.a_to_b.lock().push(msg)
        } else {
            channel.b_to_a.lock().push(msg)
        }
    }

    /// Receive a message from this endpoint
    pub fn recv(&self, channel: &Channel) -> Option<Message> {
        if self.is_side_a {
            channel.b_to_a.lock().pop()
        } else {
            channel.a_to_b.lock().pop()
        }
    }
}

/// Next channel ID
static NEXT_CHANNEL_ID: Mutex<u64> = Mutex::new(1);

/// Create a new channel between two processes
pub fn create_channel(pid_a: u64, pid_b: u64) -> (Channel, ChannelEndpoint, ChannelEndpoint) {
    let id = {
        let mut next = NEXT_CHANNEL_ID.lock();
        let id = *next;
        *next += 1;
        id
    };

    let channel = Channel {
        id,
        a_to_b: Mutex::new(RingBuffer::new()),
        b_to_a: Mutex::new(RingBuffer::new()),
        pid_a,
        pid_b,
    };

    let ep_a = ChannelEndpoint {
        channel_id: id,
        is_side_a: true,
    };

    let ep_b = ChannelEndpoint {
        channel_id: id,
        is_side_a: false,
    };

    (channel, ep_a, ep_b)
}

/// Buffer limits for IPC channels
pub const IPC_BUFFER_LIMIT: usize = 65536;
