import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";

import {
  createEvidence,
  createHypothesis,
  type Evidence,
  type Hypothesis,
} from "./evidence.ts";

export interface Investigation {
  id: string;
  symptom: string;
  capture: string;
  targetPixel?: { x: number; y: number };
  evidence: Evidence[];
  hypotheses: Hypothesis[];
  patch?: { files: string[]; summary: string };
  verification?: { capture: string; passed: boolean; details: unknown };
}

export class InvestigationValidationError extends Error {
  readonly issues: readonly string[];

  constructor(issues: readonly string[]) {
    super(`Invalid investigation:\n- ${issues.join("\n- ")}`);
    this.name = "InvestigationValidationError";
    this.issues = issues;
  }
}

export function createInvestigation(
  input: Pick<Investigation, "id" | "symptom" | "capture"> & Partial<Pick<Investigation, "targetPixel">>,
): Investigation {
  return {
    ...input,
    evidence: [],
    hypotheses: [],
  };
}

export function addEvidence(
  investigation: Investigation,
  input: Omit<Evidence, "id"> & { id?: string },
): Evidence {
  const evidence = createEvidence(input, investigation.evidence);
  investigation.evidence.push(evidence);
  return evidence;
}

export function addHypothesis(
  investigation: Investigation,
  input: Omit<Hypothesis, "id"> & { id?: string },
): Hypothesis {
  const hypothesis = createHypothesis(input, investigation.hypotheses);
  investigation.hypotheses.push(hypothesis);
  return hypothesis;
}

function nonEmpty(value: string): boolean {
  return value.trim().length > 0;
}

function duplicates(ids: readonly string[]): string[] {
  const seen = new Set<string>();
  const duplicateIds = new Set<string>();
  for (const id of ids) {
    if (seen.has(id)) duplicateIds.add(id);
    seen.add(id);
  }
  return [...duplicateIds];
}

export function validateInvestigation(investigation: Investigation): void {
  const issues: string[] = [];
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(investigation.id)) {
    issues.push("id must be a path-safe identifier of at most 128 characters");
  }
  if (!nonEmpty(investigation.symptom)) issues.push("symptom cannot be empty");
  if (!nonEmpty(investigation.capture)) issues.push("capture cannot be empty");
  if (investigation.targetPixel !== undefined) {
    const { x, y } = investigation.targetPixel;
    if (!Number.isSafeInteger(x) || x < 0 || !Number.isSafeInteger(y) || y < 0) {
      issues.push("targetPixel coordinates must be non-negative safe integers");
    }
  }

  const evidenceIds = investigation.evidence.map((evidence) => evidence.id);
  const duplicateEvidence = duplicates(evidenceIds);
  if (duplicateEvidence.length > 0) issues.push(`duplicate Evidence IDs: ${duplicateEvidence.join(", ")}`);
  const evidenceSet = new Set(evidenceIds);
  for (const evidence of investigation.evidence) {
    if (!/^E[A-Za-z0-9._-]+$/.test(evidence.id)) issues.push(`invalid Evidence ID: ${evidence.id}`);
    if (!nonEmpty(evidence.description)) issues.push(`${evidence.id} description cannot be empty`);
  }

  const hypothesisIds = investigation.hypotheses.map((hypothesis) => hypothesis.id);
  const duplicateHypotheses = duplicates(hypothesisIds);
  if (duplicateHypotheses.length > 0) issues.push(`duplicate Hypothesis IDs: ${duplicateHypotheses.join(", ")}`);
  for (const hypothesis of investigation.hypotheses) {
    if (!/^H[A-Za-z0-9._-]+$/.test(hypothesis.id)) issues.push(`invalid Hypothesis ID: ${hypothesis.id}`);
    if (!nonEmpty(hypothesis.claim)) issues.push(`${hypothesis.id} claim cannot be empty`);
    if (!nonEmpty(hypothesis.expectedBehavior)) issues.push(`${hypothesis.id} expectedBehavior cannot be empty`);
    if (!nonEmpty(hypothesis.verification)) issues.push(`${hypothesis.id} verification method cannot be empty`);
    if (hypothesis.evidenceIds.length === 0) issues.push(`${hypothesis.id} must reference at least one Evidence ID`);
    for (const evidenceId of hypothesis.evidenceIds) {
      if (!evidenceSet.has(evidenceId)) issues.push(`${hypothesis.id} references missing Evidence ID ${evidenceId}`);
    }
    if (hypothesis.status === "confirmed") {
      if (investigation.verification?.passed !== true) {
        issues.push(`${hypothesis.id} cannot be confirmed without passed investigation verification`);
      }
      if (!nonEmpty(investigation.verification?.capture ?? "")) {
        issues.push(`${hypothesis.id} cannot be confirmed without a verification capture`);
      } else if (resolve(investigation.verification!.capture) === resolve(investigation.capture)) {
        issues.push(`${hypothesis.id} verification capture must differ from the original capture`);
      }
    }
  }

  if (investigation.patch !== undefined) {
    if (investigation.patch.files.length === 0) issues.push("patch must name at least one file");
    if (!nonEmpty(investigation.patch.summary)) issues.push("patch summary cannot be empty");
  }
  if (issues.length > 0) throw new InvestigationValidationError(issues);
}

function title(value: string): string {
  return value.split("-").map((word) => word.charAt(0).toUpperCase() + word.slice(1)).join(" ");
}

function dataBlock(data: unknown): string {
  let serialized: string;
  try {
    serialized = JSON.stringify(data, null, 2) ?? "null";
  } catch {
    serialized = JSON.stringify({ unavailable: "Evidence data is not JSON-serializable" }, null, 2);
  }
  const maxLength = 8_000;
  if (serialized.length > maxLength) {
    serialized = `${serialized.slice(0, maxLength)}\n... [report data truncated; see investigation.json]`;
  }
  return `\`\`\`json\n${serialized}\n\`\`\``;
}

export function renderInvestigationReport(investigation: Investigation): string {
  validateInvestigation(investigation);
  const drawEvidence = investigation.evidence.find((evidence) => evidence.kind === "draw");
  const sourceEvidence = investigation.evidence.filter((evidence) => evidence.kind === "source-code");
  const confirmed = investigation.hypotheses.filter((hypothesis) => hypothesis.status === "confirmed");
  const lines: string[] = [
    `# Investigation: ${investigation.id}`,
    "",
    "## Symptom",
    "",
    investigation.symptom,
    "",
    "## Capture",
    "",
    investigation.capture,
    "",
    "## Responsible Draw",
    "",
    drawEvidence === undefined ? "Not yet identified." : `${drawEvidence.id}: ${drawEvidence.description}\n\n${dataBlock(drawEvidence.data)}`,
    "",
    "## Evidence",
    "",
  ];
  for (const evidence of investigation.evidence) {
    lines.push(
      `### ${evidence.id} — ${title(evidence.kind)}`,
      "",
      evidence.description,
      "",
      `Source: ${evidence.source}`,
      "",
      dataBlock(evidence.data),
      "",
    );
  }
  lines.push("## Hypothesis", "");
  for (const hypothesis of investigation.hypotheses) {
    lines.push(
      `### ${hypothesis.id}`,
      "",
      `Claim: ${hypothesis.claim}`,
      "",
      "Evidence:",
      ...hypothesis.evidenceIds.map((id) => `- ${id}`),
      "",
      `Expected behavior: ${hypothesis.expectedBehavior}`,
      "",
      `Verification method: ${hypothesis.verification}`,
      "",
      `Confidence: ${title(hypothesis.confidence)}`,
      "",
      `Status: ${hypothesis.status}`,
      "",
    );
  }
  lines.push("## Source Mapping", "");
  if (sourceEvidence.length === 0) {
    lines.push("No source mapping recorded.", "");
  } else {
    for (const evidence of sourceEvidence) {
      lines.push(`${evidence.id}: ${evidence.description}`, "", dataBlock(evidence.data), "");
    }
  }
  lines.push(
    "## Patch",
    "",
    investigation.patch === undefined
      ? "No patch recorded."
      : `${investigation.patch.summary}\n\nFiles:\n${investigation.patch.files.map((file) => `- ${file}`).join("\n")}`,
    "",
    "## Verification",
    "",
    investigation.verification === undefined
      ? "Not yet verified."
      : `Fixed capture: ${investigation.verification.capture}\n\nResult: ${investigation.verification.passed ? "PASS" : "FAIL"}\n\n${dataBlock(investigation.verification.details)}`,
    "",
    "## Final Result",
    "",
    confirmed.length === 0
      ? "No root cause has been confirmed."
      : confirmed.map((hypothesis) => `${hypothesis.id}: ${hypothesis.claim} (Evidence: ${hypothesis.evidenceIds.join(", ")})`).join("\n\n"),
    "",
  );
  return lines.join("\n");
}

export async function writeInvestigation(
  investigation: Investigation,
  rootDirectory = resolve(process.cwd(), "investigations"),
): Promise<{ directory: string; jsonPath: string; reportPath: string }> {
  validateInvestigation(investigation);
  const directory = resolve(rootDirectory, investigation.id);
  await mkdir(directory, { recursive: true });
  const jsonPath = resolve(directory, "investigation.json");
  const reportPath = resolve(directory, "report.md");
  await writeFile(jsonPath, `${JSON.stringify(investigation, null, 2)}\n`, "utf8");
  await writeFile(reportPath, renderInvestigationReport(investigation), "utf8");
  return { directory, jsonPath, reportPath };
}
