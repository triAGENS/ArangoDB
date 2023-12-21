set -e

manifest=$1
tag=$2

echo "Creating docker multiarch manifest \"${manifest}:${tag}\":"
# set +e; docker manifest rm -f ${manifest}:${IMAGE_TAG} 2>/dev/null; set -e
docker manifest create ${manifest}:${tag} \
  --amend ${manifest}:${tag}-amd64 \
  --amend ${manifest}:${tag}-arm64 \
|| ( echo "Error during docker multiarch manifest creation!"; exit 1 )

docker manifest push --purge ${manifest}:${tag}
