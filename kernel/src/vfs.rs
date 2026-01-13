
/// VFS Node trait
pub trait VfsNode {
    fn read(&self, offset: u64, buf: &mut [u8]) -> Result<usize, ()>;
    fn write(&mut self, offset: u64, buf: &[u8]) -> Result<usize, ()>;
}
