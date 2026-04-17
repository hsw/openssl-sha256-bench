variable "VERSIONS" {
  default = [
    "3.0.20",
    "3.1.8",
    "3.2.6",
    "3.3.7",
    "3.4.5",
    "3.5.6",
    "3.6.2",
    "4.0.0",
  ]
}

group "default" {
  targets = ["bench"]
}

target "bench" {
  name       = "bench-${replace(version, ".", "_")}"
  matrix     = { version = VERSIONS }
  dockerfile = "Dockerfile"
  context    = "."
  args       = { OPENSSL_VERSION = version }
  tags       = ["bench-sha256:${version}"]
  output     = ["type=docker"]
}
