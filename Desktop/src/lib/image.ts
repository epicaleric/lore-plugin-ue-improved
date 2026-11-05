import { api } from "./api";

/** A square png of a picture (the middle of it), as base64. */
export async function squarePng(path: string, side = 256): Promise<string> {
  const picture = await api.readImage(path);
  if (!picture) throw new Error("That image could not be read.");
  const image = await new Promise<HTMLImageElement>((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error("That image could not be opened."));
    img.src = picture;
  });
  const canvas = document.createElement("canvas");
  canvas.width = side;
  canvas.height = side;
  const crop = Math.min(image.width, image.height);
  canvas.getContext("2d")!.drawImage(image, (image.width - crop) / 2, (image.height - crop) / 2, crop, crop, 0, 0, side, side);
  return canvas.toDataURL("image/png").split(",")[1];
}

/** Where Unreal keeps a project's thumbnail: next to the .uproject, with the same name. */
export const thumbnailPathOf = (uproject: string) => uproject.replace(/\\/g, "/").replace(/\.uproject$/i, ".png");
