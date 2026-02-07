use vulture_kernel::Kernel;

fn main() {
    println!("pranaOS Bootloader");

    // Fake memory map for now
    let memory_map = vec![
        ("RAM", 0x0000_0000, 0x1_0000_0000),
        ("IO",  0xFE00_0000, 0x1000_0000),
    ];

    println!("Memory Map:");
    for (t, s, e) in &memory_map {
        println!("  {}: {:#x} -> {:#x}", t, s, e);
    }

    println!("➡ Jumping to kernel...\n");

    Kernel::new(memory_map).run();
}