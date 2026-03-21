import { build } from "esbuild";

const watch = process.argv.includes("--watch");

await build({
  entryPoints: ["src/main.ts"],
  bundle: true,
  format: "cjs",
  platform: "browser",
  target: "es2022",
  sourcemap: watch ? "inline" : false,
  outfile: "main.js",
  external: ["obsidian", "@codemirror/state"],
  logLevel: "info",
  ...(watch ? { watch: true } : {}),
});
