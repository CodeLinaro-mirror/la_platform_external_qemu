#!/bin/bash

# --- Default Configuration ---
GS_BUCKET_BASE_URL="gs://emu-dev-development" # Base URL for SDKs
TEMP_DOWNLOAD_DIR="/tmp"
SDK_VERSION="" # To be set by command-line argument

# --- Helper Functions ---
log_info() {
  echo "INFO: $1"
}

log_warn() {
  echo "WARN: $1"
}

log_error_and_exit() {
  echo "ERROR: $1"
  exit 1
}

check_command() {
  if ! command -v "$1" &> /dev/null; then
    local install_info=""
    if [ "$1" == "gcloud" ] || [ "$1" == "gsutil" ]; then
      install_info="For gcloud/gsutil, see: https://cloud.google.com/sdk/docs/install"
    fi
    log_error_and_exit "Required command '$1' not found. Please install it and ensure it's in your PATH. ${install_info}"
  fi
}

# --- Argument Parsing ---
if [ "$#" -ne 2 ]; then
    log_error_and_exit "Usage: $0 --sdk-version <version> (e.g., --sdk-version 13.3)"
fi

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --sdk-version) SDK_VERSION="$2"; shift ;;
        *) log_error_and_exit "Unknown parameter passed: $1. Usage: $0 --sdk-version <version>" ;;
    esac
    shift
done

if [ -z "$SDK_VERSION" ]; then
    log_error_and_exit "SDK version not provided. Usage: $0 --sdk-version <version>"
fi

log_info "Targeting SDK Version: ${SDK_VERSION}"

# --- Dynamic Path Configuration ---
SDK_ZIP_NAME="MacOSX${SDK_VERSION}.sdk.zip"
SDK_NAME="MacOSX${SDK_VERSION}.sdk"
SDK_URL="${GS_BUCKET_BASE_URL}/${SDK_ZIP_NAME}"

# --- Pre-flight Checks ---
log_info "--- Starting SDK Installation Script for macOS SDK ${SDK_VERSION} ---"

# 1. Check for gcloud and attempt login
log_info "1. Checking for gcloud CLI and ensuring authentication..."
check_command "gcloud"
log_info "   gcloud CLI found."
log_info "   Attempting Google Cloud authentication (this may open a browser window)..."
if ! gcloud auth login; then
    log_error_and_exit "Google Cloud authentication failed. Please ensure you can authenticate with 'gcloud auth login' successfully."
fi
# Optional: Add 'gcloud config set project YOUR_PROJECT_ID' if needed for gsutil permissions,
# but gsutil cp often works with bucket-level permissions without a project explicitly set after login.
log_info "   Successfully authenticated with Google Cloud or already authenticated."


# 2. Determine Xcode Developer Path
log_info "2. Determining Xcode developer path..."
XCODE_DEV_PATH=$(xcode-select -print-path 2>/dev/null)

if [ -z "$XCODE_DEV_PATH" ] || [ ! -d "$XCODE_DEV_PATH" ]; then
  log_error_and_exit "Could not determine a valid Xcode developer path using 'xcode-select -print-path'. Ensure Xcode and its Command Line Tools are installed and properly selected (e.g., via 'sudo xcode-select -s /Applications/Xcode.app/Contents/Developer')."
fi
log_info "   Xcode Developer Path: ${XCODE_DEV_PATH}"

# 3. Derive and Check Xcode SDK Directory
XCODE_SDK_DIR="${XCODE_DEV_PATH}/Platforms/MacOSX.platform/Developer/SDKs/"
log_info "3. Checking Xcode SDK directory: ${XCODE_SDK_DIR}"

if [ ! -d "${XCODE_SDK_DIR}" ]; then
  log_warn "Target Xcode SDK directory does not exist: ${XCODE_SDK_DIR}"
  log_warn "This script can create it, but ensure the base path (${XCODE_DEV_PATH}/Platforms/MacOSX.platform/Developer/) is valid."
fi
log_info "   Using Xcode SDK Directory: ${XCODE_SDK_DIR}"

# 4. Check for gsutil (gcloud auth login implies gsutil will use the same auth)
log_info "4. Checking for gsutil..."
check_command "gsutil" # gsutil is part of the Google Cloud SDK
log_info "   gsutil found."

# 5. Check for unzip
log_info "5. Checking for unzip..."
check_command "unzip"
log_info "   unzip found."

# --- Main Operations ---
TEMP_FILE_PATH="${TEMP_DOWNLOAD_DIR}/${SDK_ZIP_NAME}"

# 6. Download the SDK zip file
log_info "6. Downloading SDK from ${SDK_URL} to ${TEMP_FILE_PATH}..."
if gsutil cp "${SDK_URL}" "${TEMP_FILE_PATH}"; then
  log_info "   Download successful."
else
  log_error_and_exit "Failed to download SDK from ${SDK_URL}. Check the URL, SDK version, your gsutil access, and ensure you are authenticated to the correct Google Cloud account with permissions for this bucket."
fi

# 7. Unzip the SDK to the Xcode directory
TARGET_SDK_PATH="${XCODE_SDK_DIR}${SDK_NAME}"
log_info "7. Unzipping ${TEMP_FILE_PATH} to ${TARGET_SDK_PATH}..."

if [ -d "${TARGET_SDK_PATH}" ]; then
  log_warn "SDK directory '${TARGET_SDK_PATH}' already exists."
  read -r -p "   Overwrite? (y/N): " confirm_overwrite
  if [[ "$confirm_overwrite" != "y" && "$confirm_overwrite" != "Y" ]]; then
    log_info "   Skipping unzip. Cleaning up downloaded file."
    rm -f "${TEMP_FILE_PATH}"
    log_info "--- Script Finished (User cancelled overwrite) ---"
    exit 0
  fi
  log_info "   Overwriting existing SDK directory..."
  SUDO_RM_CMD=""
  if [ ! -w "$(dirname "${TARGET_SDK_PATH}")" ] || ( [ -e "${TARGET_SDK_PATH}" ] && [ ! -w "${TARGET_SDK_PATH}" ] ); then
      log_info "   Elevating privileges with sudo to remove existing SDK directory..."
      SUDO_RM_CMD="sudo"
  fi
  if ! ${SUDO_RM_CMD} rm -rf "${TARGET_SDK_PATH}"; then
      log_error_and_exit "Failed to remove existing SDK directory '${TARGET_SDK_PATH}'. Check permissions."
  fi
fi

# Create parent SDK directory if it doesn't exist, with sudo if necessary
if [ ! -d "${XCODE_SDK_DIR}" ]; then
    log_info "   Xcode SDK directory ${XCODE_SDK_DIR} does not exist. Attempting to create it."
    SUDO_MKDIR_CMD=""
    # Check if parent of XCODE_SDK_DIR is writable
    PARENT_OF_XCODE_SDK_DIR=$(dirname "${XCODE_SDK_DIR}")
    if [ ! -w "${PARENT_OF_XCODE_SDK_DIR}" ] ; then
        log_info "   Elevating privileges with sudo to create SDK directory ${XCODE_SDK_DIR}..."
        SUDO_MKDIR_CMD="sudo"
    fi
    if ! ${SUDO_MKDIR_CMD} mkdir -p "${XCODE_SDK_DIR}"; then
        log_error_and_exit "Failed to create SDK directory '${XCODE_SDK_DIR}'."
    fi
fi


# Attempt to unzip. Use sudo if the target directory requires it.
log_info "   Extracting ${SDK_ZIP_NAME}..."
SUDO_UNZIP_CMD=""
# Check if XCODE_SDK_DIR itself is writable
if [ ! -w "${XCODE_SDK_DIR}" ]; then
    log_info "   Elevating privileges with sudo to unzip into ${XCODE_SDK_DIR}..."
    SUDO_UNZIP_CMD="sudo"
fi

if ${SUDO_UNZIP_CMD} unzip -q "${TEMP_FILE_PATH}" -d "${XCODE_SDK_DIR}"; then
  log_info "   Unzip successful."
else
  log_error_and_exit "Failed to unzip SDK to ${XCODE_SDK_DIR}. Ensure the zip file is valid, contains the '${SDK_NAME}' folder, and you have permissions."
fi

# Verify the SDK folder was created
if [ ! -d "${TARGET_SDK_PATH}" ]; then
    log_error_and_exit "SDK directory '${TARGET_SDK_PATH}' was not found after unzip."
fi
log_info "   SDK folder '${SDK_NAME}' successfully extracted to Xcode."

# 8. Clean up the downloaded zip file
log_info "8. Cleaning up downloaded file: ${TEMP_FILE_PATH}..."
rm -f "${TEMP_FILE_PATH}"
log_info "   Cleanup successful."

log_info "--- SDK Installation Script Finished Successfully ---"
log_warn "#####################################################################"
log_warn "# IMPORTANT: Manually adding SDKs to Xcode is UNSUPPORTED by Apple. #"
log_warn "# This can lead to Xcode instability or build issues.               #"
log_warn "# Consider installing multiple Xcode versions as a safer            #"
log_warn "# alternative for using different SDKs.                             #"
log_warn "# You may need to restart Xcode for changes to be recognized.       #"
log_warn "#####################################################################"