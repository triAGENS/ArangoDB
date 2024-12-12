import { Database } from "arangojs";
import { memoize } from "lodash";

export const getDB = memoize(
  (db: string) =>
    new Database({
      url: window.location.origin + window.frontendConfig.basePath,
      databaseName: db,
      auth: {
        token: window.arangoHelper.getCurrentJwt()
      }
    })
);

export const getCurrentDB = () => getDB(window.frontendConfig.db);
export const getRouteForDB = memoize(
  (db: string, route: string) => getDB(db).route(route),
  (db: string, route: string) => `${db}/${route}`
);

export const getRouteForCurrentDB = (route: string) => {
  return getRouteForDB(window.frontendConfig.db, route);
};

export const getAardvarkRouteForCurrentDb = (url: string) =>
  getRouteForDB(window.frontendConfig.db, `_admin/aardvark/${url}`);
export const getApiRouteForCurrentDB = () =>
  getRouteForDB(window.frontendConfig.db, "_api");
export const getAdminRouteForCurrentDB = () =>
  getRouteForDB(window.frontendConfig.db, "_admin");
