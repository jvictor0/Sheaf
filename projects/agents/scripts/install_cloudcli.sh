#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
vendor_dir="${project_dir}/vendor/cloudcli/tree"
manifest="${project_dir}/vendor/cloudcli/VENDOR.toml"
patch_dir="${project_dir}/vendor/cloudcli/patches"
prefix="${CLOUDCLI_PREFIX:-${HOME}/.local}"
port="${CLOUDCLI_PORT:-3001}"
unit_dir="${HOME}/.config/systemd/user"
unit_path="${unit_dir}/cloudcli.service"
data_dir="${HOME}/.cloudcli"
build_dir=""

die() {
    echo "error: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

cloudcli_host() {
    local host="${CLOUDCLI_HOST:-127.0.0.1}"
    [[ "${host}" =~ ^127\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$ ]] ||
        die "CLOUDCLI_HOST must be a loopback IPv4 address (got '${host}')"
    printf '%s\n' "${host}"
}

tailscale_dns_name() {
    local dns_name

    require_command tailscale
    dns_name="$(tailscale status --json | sed -n 's/^[[:space:]]*"DNSName": "\([^"]*\)",[[:space:]]*$/\1/p' | sed -n '1p')"
    dns_name="${dns_name%.}"
    [[ -n "${dns_name}" ]] || die "could not determine this node's Tailscale DNS name"
    printf '%s\n' "${dns_name}"
}

configure_tailscale_serve() {
    local host="$1"
    local serve_status

    serve_status="$(tailscale serve status 2>/dev/null || true)"
    if grep -Fq "proxy http://${host}:${port}" <<<"${serve_status}"; then
        echo "Tailscale Serve already proxies HTTPS port 443 to http://${host}:${port}"
        return
    fi

    echo "Publishing CloudCLI through Tailscale Serve on HTTPS port 443..."
    tailscale serve --bg --yes "http://${host}:${port}"
}

expected_version() {
    sed -n 's/^version = "\([^"]*\)"$/\1/p' "${manifest}"
}

write_service() {
    local host="$1"
    local temporary_unit

    mkdir -p "${unit_dir}"
    temporary_unit="$(mktemp "${unit_dir}/.cloudcli.service.XXXXXX")"
    {
        echo "[Unit]"
        echo "Description=CloudCLI web UI (Sheaf vendored build)"
        echo "After=network-online.target"
        echo "Wants=network-online.target"
        echo
        echo "[Service]"
        echo "Type=simple"
        echo "Environment=HOST=${host}"
        echo "Environment=SERVER_PORT=${port}"
        echo "Environment=PATH=${prefix}/bin:/usr/local/bin:/usr/bin:/bin"
        echo "ExecStart=${prefix}/bin/cloudcli start"
        echo "Restart=on-failure"
        echo "RestartSec=5"
        echo
        echo "[Install]"
        echo "WantedBy=default.target"
    } >"${temporary_unit}"
    chmod 0644 "${temporary_unit}"
    mv "${temporary_unit}" "${unit_path}"
}

check_installation() {
    local host="$1"
    local wanted_version actual_version environment runtime_path
    local dns_name serve_status tailscale_ip

    wanted_version="$(expected_version)"
    [[ -n "${wanted_version}" ]] || die "could not read version from ${manifest}"
    [[ -x "${prefix}/bin/cloudcli" ]] ||
        die "CloudCLI executable is missing at ${prefix}/bin/cloudcli"

    actual_version="$("${prefix}/bin/cloudcli" --version 2>&1 | tail -n 1)"
    [[ "${actual_version}" == "${wanted_version}" ]] ||
        die "installed CloudCLI version is ${actual_version}; expected ${wanted_version}"

    runtime_path="${prefix}/lib/node_modules/@cloudcli-ai/cloudcli/dist-server/server/modules/providers/list/codex/codex-runtime.provider.js"
    grep -q "approvals_reviewer" "${runtime_path}" ||
        die "installed CloudCLI is missing Sheaf's Approve-for-me patch"

    systemctl --user is-active --quiet cloudcli.service ||
        die "cloudcli.service is not active"
    environment="$(systemctl --user show cloudcli.service --property=Environment --value)"
    [[ " ${environment} " == *" HOST=${host} "* ]] ||
        die "cloudcli.service is not bound to ${host}"
    [[ " ${environment} " == *" SERVER_PORT=${port} "* ]] ||
        die "cloudcli.service is not configured for port ${port}"

    curl --fail --silent --show-error "http://${host}:${port}/health" >/dev/null ||
        die "CloudCLI health check failed at http://${host}:${port}/health"

    dns_name="$(tailscale_dns_name)"
    tailscale_ip="$(tailscale ip -4 | sed -n '1p')"
    serve_status="$(tailscale serve status)"
    grep -Fq "proxy http://${host}:${port}" <<<"${serve_status}" ||
        die "Tailscale Serve is not proxying HTTPS to http://${host}:${port}"
    [[ -n "${tailscale_ip}" ]] || die "could not determine this node's Tailscale IPv4 address"
    curl --fail --silent --show-error --resolve "${dns_name}:443:${tailscale_ip}" "https://${dns_name}/health" >/dev/null ||
        die "CloudCLI health check failed at https://${dns_name}/health"

    echo "CloudCLI ${actual_version} is active at https://${dns_name}/"
    echo "The backend is restricted to http://${host}:${port}"
    echo "Persistent data remains in ${data_dir}"
}

install_cloudcli() {
    local host="$1"
    local package_dir package_path source_dir patch_file
    local attempt

    require_command npm
    require_command git
    require_command tar
    require_command systemctl
    require_command curl
    [[ -f "${vendor_dir}/package.json" ]] ||
        die "CloudCLI submodule is missing; run git submodule update --init"
    [[ -f "${manifest}" ]] || die "vendor manifest is missing: ${manifest}"

    build_dir="$(mktemp -d)"
    trap 'rm -rf -- "${build_dir}"' EXIT
    source_dir="${build_dir}/source"
    package_dir="${build_dir}/package"
    mkdir -p "${source_dir}" "${package_dir}"
    git -C "${vendor_dir}" archive HEAD | tar -x -C "${source_dir}"

    for patch_file in "${patch_dir}"/*.patch; do
        echo "Applying $(basename "${patch_file}")..."
        (
            cd "${source_dir}"
            git apply --check --unidiff-zero "${patch_file}"
            git apply --unidiff-zero "${patch_file}"
        )
    done

    echo "Building patched vendored CloudCLI $(expected_version)..."
    (
        cd "${source_dir}"
        HUSKY=0 npm ci
        npm run build
    )

    (cd "${source_dir}" && npm pack --ignore-scripts --silent --pack-destination "${package_dir}" >/dev/null)
    package_path="$(find "${package_dir}" -maxdepth 1 -type f -name '*.tgz' -print -quit)"
    [[ -n "${package_path}" && -f "${package_path}" ]] ||
        die "npm pack did not create a package archive"

    echo "Replacing the existing application package; preserving ${data_dir}..."
    systemctl --user stop cloudcli.service 2>/dev/null || true
    npm install --global --prefix "${prefix}" "${package_path}"

    write_service "${host}"
    systemctl --user daemon-reload
    systemctl --user enable cloudcli.service
    systemctl --user restart cloudcli.service
    configure_tailscale_serve "${host}"

    for attempt in {1..30}; do
        if curl --fail --silent "http://${host}:${port}/health" >/dev/null 2>&1; then
            check_installation "${host}"
            return
        fi
        sleep 1
    done

    systemctl --user status cloudcli.service --no-pager || true
    die "CloudCLI did not become healthy within 30 seconds"
}

main() {
    local action="${1:-install}"
    local host

    host="$(cloudcli_host)"
    case "${action}" in
        install) install_cloudcli "${host}" ;;
        check) check_installation "${host}" ;;
        *) die "usage: $0 [install|check]" ;;
    esac
}

main "$@"
