#!/usr/bin/env python
import pytest
import requests
import requests_cache

# patch requests to use cache
@pytest.fixture(scope="session", autouse=True)
def cache_requests():
    requests_cache.install_cache('testing_cache')
