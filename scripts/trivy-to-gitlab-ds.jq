# Convert Trivy's native JSON output (from `trivy sbom --format json` against
# a CycloneDX SBOM of frontend/bun.lock) into a gemnasium v15.0.7
# dependency_scanning report that GitLab's MR security widget accepts.
#
# Trivy's shipped @/contrib/gitlab.tpl is hardcoded for container_scanning
# (the schema validator rejects it on `/scan/type` and a missing
# `dependency_files` key), so this filter does the equivalent shaping for
# dependency_scanning instead.
#
# Severity values are case-sensitive in the gemnasium schema (Critical,
# High, Medium, Low, Unknown). Trivy outputs uppercase (CRITICAL, HIGH...).
#
# `package_manager` is one of the values from the gemnasium enum; "npm" is
# the closest match for bun.lock since bun is npm-compatible at the
# package-resolution level. The location.file is hardcoded to
# `frontend/bun.lock` because Trivy's SBOM-mode Target is the language name
# ("Node.js"), not the source lockfile path.

{
  version: "15.0.7",
  scan: {
    analyzer: {
      id: "trivy",
      name: "Trivy",
      vendor: { name: "Aqua Security" },
      version: (.Trivy.Version // "unknown")
    },
    scanner: {
      id: "trivy",
      name: "Trivy",
      url: "https://github.com/aquasecurity/trivy/",
      vendor: { name: "Aqua Security" },
      version: (.Trivy.Version // "unknown")
    },
    type: "dependency_scanning",
    status: "success",
    start_time: ((.CreatedAt // "1970-01-01T00:00:00Z") | sub("\\..*"; "") | sub("Z$"; "")),
    end_time:   ((.CreatedAt // "1970-01-01T00:00:00Z") | sub("\\..*"; "") | sub("Z$"; ""))
  },
  vulnerabilities: [
    (.Results // [])[]
    | . as $r
    | (.Vulnerabilities // [])[]
    | {
        id: .VulnerabilityID,
        category: "dependency_scanning",
        name: (.Title // .VulnerabilityID),
        description: (.Description // ""),
        severity: (
          if   .Severity == "CRITICAL" then "Critical"
          elif .Severity == "HIGH"     then "High"
          elif .Severity == "MEDIUM"   then "Medium"
          elif .Severity == "LOW"      then "Low"
          else "Unknown" end
        ),
        solution: (if .FixedVersion and .FixedVersion != ""
                   then "Upgrade to " + .FixedVersion
                   else "" end),
        scanner: { id: "trivy", name: "Trivy" },
        identifiers: [
          {
            type: "cve",
            name: .VulnerabilityID,
            value: .VulnerabilityID,
            url: (.PrimaryURL // "")
          }
        ],
        links: [ (.References // [])[] | { url: . } ],
        location: {
          file: "frontend/bun.lock",
          dependency: {
            package: { name: .PkgName },
            version: .InstalledVersion
          }
        }
      }
  ],
  dependency_files: [
    (.Results // [])[]
    | select(.Class == "lang-pkgs")
    | {
        path: "frontend/bun.lock",
        package_manager: "npm",
        dependencies: [
          (.Packages // [])[]
          | {
              package: { name: .Name },
              version: .Version
            }
        ]
      }
  ],
  remediations: []
}
