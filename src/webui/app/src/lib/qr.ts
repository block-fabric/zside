// A QR code as a PNG picture, with the address under it (QRImageWidget::setQR, saveImage).
import { encode } from 'uqr';

/** The PNG, as base64; null where the text does not fit a QR code or there is no canvas. */
export function qrPngBase64(text: string, caption: string, scale = 8): string | null {
  let qr;
  try {
    qr = encode(text, { ecc: 'M', border: 2 });
  } catch {
    return null;
  }
  const size = qr.size * scale;
  const captionHeight = caption ? 34 : 0;
  const canvas = document.createElement('canvas');
  canvas.width = size;
  canvas.height = size + captionHeight;
  const ctx = canvas.getContext('2d');
  if (!ctx) return null;
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = '#000000';
  qr.data.forEach((row, y) => row.forEach((on, x) => on && ctx.fillRect(x * scale, y * scale, scale, scale)));
  if (caption) {
    ctx.font = '14px monospace';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    // A long address is made smaller to fit.
    const width = ctx.measureText(caption).width;
    if (width > size - 12) ctx.font = `${Math.max(8, Math.floor((14 * (size - 12)) / width))}px monospace`;
    ctx.fillText(caption, size / 2, size + captionHeight / 2 - 2);
  }
  const url = canvas.toDataURL('image/png');
  const comma = url.indexOf(',');
  return comma >= 0 ? url.slice(comma + 1) : null;
}
