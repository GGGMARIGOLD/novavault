//! vespera-img: resizes and recompresses an uploaded image for Vespera.
//!
//! Usage: vespera-img <avatar|banner|cover> <input> <output-without-extension>
//! Prints the extension it wrote ("jpg" or "png") on success. On any failure it
//! exits with a non-zero code and the server keeps the original upload.

use image::codecs::jpeg::JpegEncoder;
use image::imageops::FilterType;
use image::{DynamicImage, GenericImageView, ImageReader, Limits};
use std::fs::File;
use std::io::BufWriter;
use std::process::ExitCode;

/// Largest size each kind of image is shown at (2x for sharp high-DPI screens).
fn max_size(kind: &str) -> Option<(u32, u32)> {
    match kind {
        "avatar" => Some((256, 256)),
        "banner" => Some((1600, 600)),
        "cover" => Some((1280, 720)),
        _ => None,
    }
}

/// True if any pixel is not fully opaque, so the image needs to stay PNG.
fn has_transparency(img: &DynamicImage) -> bool {
    img.color().has_alpha() && img.to_rgba8().pixels().any(|p| p[3] < 255)
}

fn run() -> Result<&'static str, String> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        return Err("usage: vespera-img <avatar|banner|cover> <input> <output-without-extension>".into());
    }
    let (w, h) = max_size(&args[1]).ok_or("unknown kind")?;

    // Refuse "decompression bombs": tiny files that expand to huge images in memory.
    let mut limits = Limits::default();
    limits.max_image_width = Some(12_000);
    limits.max_image_height = Some(12_000);
    limits.max_alloc = Some(256 * 1024 * 1024);

    let mut reader = ImageReader::open(&args[2]).map_err(|e| e.to_string())?
        .with_guessed_format().map_err(|e| e.to_string())?;
    reader.limits(limits);
    let mut img = reader.decode().map_err(|e| e.to_string())?;

    let (iw, ih) = img.dimensions();
    if iw > w || ih > h {
        img = img.resize(w, h, FilterType::Lanczos3); // keeps the aspect ratio
    }

    if has_transparency(&img) {
        let out = format!("{}.png", args[3]);
        img.to_rgba8().save(&out).map_err(|e| e.to_string())?;
        Ok("png")
    } else {
        let out = format!("{}.jpg", args[3]);
        let file = BufWriter::new(File::create(&out).map_err(|e| e.to_string())?);
        JpegEncoder::new_with_quality(file, 84).encode_image(&img.to_rgb8()).map_err(|e| e.to_string())?;
        Ok("jpg")
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(ext) => { println!("{ext}"); ExitCode::SUCCESS }
        Err(e) => { eprintln!("vespera-img: {e}"); ExitCode::FAILURE }
    }
}
