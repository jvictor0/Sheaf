#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
vendor_dir="${project_dir}/vendor/cloudcli/tree"
manifest="${project_dir}/vendor/cloudcli/VENDOR.toml"
prefix="${CLOUDCLI_PREFIX:-${HOME}/.local}"
port="${CLOUDCLI_PORT:-3001}"
unit_dir="${HOME}/.config/systemd/user"
unit_path="${unit_dir}/cloudcli.service"
data_dir="${HOME}/.cloudcli"
package_dir=""

die() {
    echo "error: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

tailscale_host() {
    local host="${CLOUDCLI_HOST:-}"
    if [[ -z "${host}" ]]; then
        require_command tailscale
        host="$(tailscale ip -4 | sed -n '1p')"
    fi
    [[ "${host}" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] ||
        die "CLOUDCLI_HOST must be an IPv4 address (got '${host}')"
    printf '%s\n' "${host}"
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
    local wanted_version actual_version environment

    wanted_version="$(expected_version)"
    [[ -n "${wanted_version}" ]] || die "could not read version from ${manifest}"
    [[ -x "${prefix}/bin/cloudcli" ]] ||
        die "CloudCLI executable is missing at ${prefix}/bin/cloudcli"

    actual_version="$("${prefix}/bin/cloudcli" --version 2>&1 | tail -n 1)"
    [[ "${actual_version}" == "${wanted_version}" ]] ||
        die "installed CloudCLI version is ${actual_version}; expected ${wanted_version}"

    systemctl --user is-active --quiet cloudcli.service ||
        die "cloudcli.service is not active"
    environment="$(systemctl --user show cloudcli.service --property=Environment --value)"
    [[ " ${environment} " == *" HOST=${host} "* ]] ||
        die "cloudcli.service is not bound to ${host}"
    [[ " ${environment} " == *" SERVER_PORT=${port} "* ]] ||
        die "cloudcli.service is not configured for port ${port}"

    curl --fail --silent --show-error "http://${host}:${port}/health" >/dev/null ||
        die "CloudCLI health check failed at http://${host}:${port}/health"

    echo "CloudCLI ${actual_version} is active at http://${host}:${port}"
    echo "Persistent data remains in ${data_dir}"
}

install_cloudcli() {
    local host="$1"
    local package_name package_path
    local attempt

    require_command npm
    require_command systemctl
    require_command curl
    [[ -f "${vendor_dir}/package.json" ]] ||
        die "CloudCLI submodule is missing; run git submodule update --init"
    [[ -f "${manifest}" ]] || die "vendor manifest is missing: ${manifest}"

    echo "Building vendored CloudCLI $(expected_version)..."
    (
        cd "${vendor_dir}"
        HUSKY=0 npm ci
        npm run build
    )

    package_dir="$(mktemp -d)"
    trap 'rm -rf -- "${package_dir}"' EXIT
    package_name="$(cd "${vendor_dir}" && npm pack --ignore-scripts --silent --pack-destination "${package_dir}")"
    package_path="${package_dir}/${package_name}"
    [[ -f "${package_path}" ]] || die "npm pack did not create ${package_path}"

    echo "Replacing the existing application package; preserving ${data_dir}..."
    systemctl --user stop cloudcli.service 2>/dev/null || true
    npm install --global --prefix "${prefix}" "${package_path}"

    write_service "${host}"
    systemctl --user daemon-reload
    systemctl --user enable cloudcli.service
    systemctl --user restart cloudcli.service

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

    host="$(tailscale_host)"
    case "${action}" in
        install) install_cloudcli "${host}" ;;
        check) check_installation "${host}" ;;
        *) die "usage: $0 [install|check]" ;;
    esac
}

main "$@"
