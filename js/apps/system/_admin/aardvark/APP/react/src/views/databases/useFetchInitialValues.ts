import useSWR from "swr";
import { getAdminRouteForCurrentDB } from "../../utils/arangoClient";

export const useFetchInitialValues = () => {
  const { data } = useSWR(`/databaseDefaults`, async () => {
    const route = getAdminRouteForCurrentDB();
    const res = await route.get(`server/databaseDefaults`);
    const { replicationFactor, sharding, writeConcern } = res.body;
    return {
      name: "",
      users: [],
      isOneShard: sharding === "single" || window.frontendConfig.forceOneShard,
      isSatellite: replicationFactor === "satellite",
      replicationFactor:
        replicationFactor === "satellite" ? 1 : replicationFactor,
      writeConcern
    };
  });
  return { defaults: data };
};
