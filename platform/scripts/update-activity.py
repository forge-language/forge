#!/usr/bin/env python3
"""Publish a public GitHub snapshot atomically; never invent agent counts."""
import argparse,datetime,json,pathlib,urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1]
REPOSITORY='forge-language/forge'
def fetch(path):
    request=urllib.request.Request('https://api.github.com/repos/'+REPOSITORY+path,headers={'Accept':'application/vnd.github+json','User-Agent':'Forge-public-development'})
    with urllib.request.urlopen(request,timeout=20) as response:return json.load(response)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=pathlib.Path,default=ROOT/'public-data/activity.json');args=parser.parse_args()
    repository=fetch('');pulls=fetch('/pulls?state=all&sort=updated&direction=desc&per_page=20')
    result={'updated_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'repository':repository['html_url'],
        'stats':{'stars':repository['stargazers_count'],'forks':repository['forks_count'],'open_items':repository['open_issues_count']},
        'pulls':[{'number':item['number'],'title':item['title'],'url':item['html_url'],'state':item['state'],'merged_at':item['merged_at'],'author':item['user']['login'],'labels':[label['name'] for label in item['labels']]} for item in pulls]}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    temp=args.output.with_suffix('.tmp');temp.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');temp.replace(args.output)
    print('Updated public development snapshot:',args.output)
if __name__=='__main__':main()
