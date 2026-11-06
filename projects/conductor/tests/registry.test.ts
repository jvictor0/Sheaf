import assert from "node:assert/strict";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { createRepoPaths, loadServiceRegistry } from "../src/index.js";

async function writeRegistryFile(contents: string): Promise<string>
{
  const directory = await mkdtemp(join(tmpdir(), "conductor-registry-"));
  const filePath = join(directory, "services.json");
  await writeFile(filePath, contents, "utf8");
  return filePath;
}

test("loadServiceRegistry reads the conductor service entry from config", async () =>
{
  const paths = createRepoPaths();
  const services = await loadServiceRegistry(paths.servicesJsonPath);
  const conductor = services.find((service) => service.name === "conductor");

  assert.ok(conductor);
  assert.equal(conductor.host, "0.0.0.0");
  assert.equal(conductor.port, 9001);
});

test("loadServiceRegistry rejects malformed JSON", async () =>
{
  const filePath = await writeRegistryFile("{ not valid json");

  await assert.rejects(
    () => loadServiceRegistry(filePath),
    /malformed JSON/,
  );
});

test("loadServiceRegistry rejects non-array registry", async () =>
{
  const filePath = await writeRegistryFile('{"name":"conductor"}');

  await assert.rejects(
    () => loadServiceRegistry(filePath),
    /must be a JSON array/,
  );
});

test("loadServiceRegistry rejects invalid entries", async () =>
{
  const filePath = await writeRegistryFile(JSON.stringify([
    {
      name: "broken",
      host: "127.0.0.1",
      port: "9001",
      command: "echo hi",
    },
  ]));

  await assert.rejects(
    () => loadServiceRegistry(filePath),
    /invalid port/,
  );
});

test("loadServiceRegistry rejects duplicate service names", async () =>
{
  const filePath = await writeRegistryFile(JSON.stringify([
    {
      name: "alpha",
      host: "127.0.0.1",
      port: 9001,
      command: "echo alpha",
    },
    {
      name: "alpha",
      host: "127.0.0.1",
      port: 9002,
      command: "echo alpha-again",
    },
  ]));

  await assert.rejects(
    () => loadServiceRegistry(filePath),
    /duplicate service name: alpha/,
  );
});

test("loadServiceRegistry accepts optional home_path", async () =>
{
  const filePath = await writeRegistryFile(JSON.stringify([
    {
      name: "ui",
      host: "127.0.0.1",
      port: 8080,
      command: "npm start",
      home_path: "/app",
    },
  ]));

  const services = await loadServiceRegistry(filePath);
  assert.deepEqual(services[0], {
    name: "ui",
    host: "127.0.0.1",
    port: 8080,
    command: "npm start",
    home_path: "/app",
  });
});
